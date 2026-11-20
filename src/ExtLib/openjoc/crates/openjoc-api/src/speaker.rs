use super::{
    BridgeControlAssembler, DecodedAccessUnitPcm, DecodedJocAccessUnitPcm, DecodedPayloadFrame,
    DownmixPolicy, FINAL_LINKED_GAIN_BLOCK_SAMPLES, FinalLinkedGain, FinalLinkedGainError,
    JocSpatialBridge, JocSpatialFrameBridge, OpenJocError, PendingRenderFrame, ReconstructionBasis,
    ReconstructionOutputTimeline, RenderedBlock, SemanticChannelLayout, SpeakerLayout,
    SpeakerRenderer, VecDeque, add_stereo_base_downmix, base_coordinate,
};

impl SpeakerRenderer {
    pub(super) fn new_with_linked_gain(
        layout: SpeakerLayout,
        downmix_policy: DownmixPolicy,
        linked_gain_enabled: bool,
        common_profile_stereo_enabled: bool,
    ) -> Self {
        let dimensions = layout.spatial().coordinate_dimension_count();
        Self {
            assembler: BridgeControlAssembler::new_with_base_projection(
                64,
                dimensions,
                !layout.is_stereo(),
            ),
            frame_bridge: JocSpatialFrameBridge,
            bridge: JocSpatialBridge::new(),
            layout,
            expected_coordinates: None,
            next_input_frame: 0,
            expected_frame: 0,
            expected_sample: 0,
            timeline: ReconstructionOutputTimeline::new(),
            pending_frames: VecDeque::new(),
            base_coordinates: None,
            downmix_policy,
            final_linked_gain: None,
            linked_gain_enabled,
            common_profile_stereo_enabled,
            #[cfg(not(target_arch = "wasm32"))]
            render_timing: super::render_timing::SpeakerTimingState::default(),
            object_output: false,
            object_state: None,
            object_ordinals: None,
            object_tracks: VecDeque::new(),
        }
    }

    /// Atmos Edition: output the scene's objects (signals + positions) instead of
    /// a speaker render. Requires a speaker layout with 3 coordinate dimensions.
    pub(super) fn set_object_output(&mut self, enabled: bool) {
        self.object_output = enabled;
    }

    pub(super) fn object_output(&self) -> bool {
        self.object_output
    }

    pub(super) fn take_object_tracks(&mut self) -> Option<Vec<super::OpenJocObjectTrack>> {
        self.object_tracks.pop_front()
    }

    pub(super) fn layout_info(&self) -> SemanticChannelLayout {
        self.layout.semantic_channel_layout()
    }

    pub(super) fn render_frame_aligned(
        &mut self,
        frame: &DecodedPayloadFrame,
        pcm_planes: &DecodedJocAccessUnitPcm,
    ) -> Result<Vec<RenderedBlock>, OpenJocError> {
        let base = &pcm_planes.joc_input_pcm;
        if frame.decoded.state_reset {
            self.timeline.reset();
            self.pending_frames.clear();
            self.assembler.reset();
            self.bridge.reset();
            if let Some(linked_gain) = self.final_linked_gain.as_mut() {
                linked_gain.reset();
            }
            self.base_coordinates = None;
            self.expected_coordinates = None;
            self.expected_frame = frame.frame_index;
            self.expected_sample = frame.sample_range.start_sample;
        }
        if frame.frame_index != self.next_input_frame {
            return Err(OpenJocError::Render(format!(
                "expected input frame {}, received {}",
                self.next_input_frame, frame.frame_index
            )));
        }
        let base_coordinates = base
            .channel_locations
            .iter()
            .copied()
            .map(base_coordinate)
            .collect::<Result<Vec<_>, _>>()?;
        let aligned = self.timeline.push_frame(
            frame.frame_index,
            frame.sample_rate,
            frame.sample_range.start_sample,
            frame.sample_range.end_sample,
            &base.channels,
            &frame.decoded.reconstruction_basis,
            base.lfe.as_deref(),
            false,
        )?;
        self.pending_frames.push_back(PendingRenderFrame {
            frame: frame.clone(),
            channel_locations: base.channel_locations.clone(),
            lfe_location: base.lfe_location,
            downmix: base.downmix,
            dialnorm: base.dialnorm,
            dialnorm_segments: base.dialnorm_segments.clone(),
            compatibility_pcm: (self.common_profile_stereo_enabled
                && self.layout.is_stereo()
                && frame.admitted_decoded_joc_binding().is_some())
            .then(|| pcm_planes.compatibility_pcm.clone()),
        });
        self.next_input_frame = self.next_input_frame.saturating_add(1);
        if let Some(previous) = &self.base_coordinates {
            if previous != &base_coordinates {
                return Err(OpenJocError::Render(
                    "Base channel topology changed within a stream".to_owned(),
                ));
            }
        } else {
            self.base_coordinates = Some(base_coordinates);
        }
        let mut rendered = Vec::new();
        for aligned_frame in aligned {
            let pending = self.pending_frames.pop_front().ok_or_else(|| {
                OpenJocError::Render("reconstruction timeline queue underflow".to_owned())
            })?;
            let aligned_base = DecodedAccessUnitPcm {
                sample_rate: aligned_frame.timeline.sample_rate,
                samples: u16::try_from(
                    aligned_frame.base_full_band_pcm.first().map_or(0, Vec::len),
                )
                .unwrap_or(u16::MAX),
                channel_locations: pending.channel_locations,
                channels: aligned_frame.base_full_band_pcm,
                lfe_location: pending.lfe_location,
                lfe: aligned_frame.lfe_pcm,
                downmix: pending.downmix,
                dialnorm: pending.dialnorm,
                dialnorm_segments: pending.dialnorm_segments,
            };
            let mut aligned_payload = pending.frame;
            aligned_payload.decoded.reconstruction_basis = aligned_frame.reconstruction_basis;
            rendered.push(self.render_aligned_block(
                &aligned_payload,
                &aligned_base,
                pending.compatibility_pcm.as_ref(),
                aligned_frame.timeline.logical_start_sample,
            )?);
        }
        Ok(rendered)
    }

    fn render_aligned_block(
        &mut self,
        frame: &DecodedPayloadFrame,
        base: &DecodedAccessUnitPcm,
        compatibility_pcm: Option<&DecodedAccessUnitPcm>,
        logical_start_sample: u64,
    ) -> Result<RenderedBlock, OpenJocError> {
        if frame.sample_range.start_sample != self.expected_sample {
            return Err(OpenJocError::Render(format!(
                "expected sample {}, received {}",
                self.expected_sample, frame.sample_range.start_sample
            )));
        }
        // Atmos Edition: owned copy (a few entries), so that `self` can be borrowed
        // mutably by the object output below.
        let base_coordinates = self
            .base_coordinates
            .clone()
            .ok_or_else(|| OpenJocError::Render("missing Base coordinate topology".to_owned()))?;
        let base_coordinates = base_coordinates.as_slice();
        if base.sample_rate != frame.sample_rate {
            return Err(OpenJocError::FormatChanged {
                expected: frame.sample_rate,
                actual: base.sample_rate,
            });
        }
        let stereo = self.layout.is_stereo();
        let common_profile_stereo = stereo
            && self.common_profile_stereo_enabled
            && frame.admitted_decoded_joc_binding().is_some();
        let calibrated_base = base.with_dialnorm_applied();
        let calibrated_compatibility = if common_profile_stereo {
            Some(
                compatibility_pcm
                    .ok_or_else(|| {
                        OpenJocError::Render(
                            "missing admitted I0 compatibility PCM plane".to_owned(),
                        )
                    })?
                    .with_dialnorm_applied(),
            )
        } else {
            None
        };
        let mut calibrated_frame = frame.clone();
        for row in &mut calibrated_frame.decoded.reconstruction_basis.rows {
            base.apply_dialnorm_to_samples(row);
        }
        let coordinate_count = base_coordinates
            .len()
            .checked_add(frame.decoded.reconstruction_basis.rows.len())
            .ok_or_else(|| OpenJocError::Render("coordinate count overflow".to_owned()))?;
        if let Some(expected) = self.expected_coordinates {
            if expected != coordinate_count {
                return Err(OpenJocError::Render(format!(
                    "coordinate count changed from {expected} to {coordinate_count}"
                )));
            }
        } else {
            self.expected_coordinates = Some(coordinate_count);
        }
        let bridge_frame = self.frame_bridge.frame(
            &calibrated_frame,
            base_coordinates,
            &calibrated_base.channels,
            calibrated_base.lfe.as_deref(),
        )?;
        let sample_count = usize::try_from(bridge_frame.sample_range.len())
            .map_err(|_| OpenJocError::Render("sample count overflow".to_owned()))?;
        let mut active =
            vec![vec![0.0; sample_count]; self.layout.spatial().active_channel_count()];
        #[cfg(not(target_arch = "wasm32"))]
        let assembly_start = super::decode_timing::start(self.render_timing.enabled);
        let control = self
            .assembler
            .assemble_frame(&calibrated_frame, base_coordinates, Some(crate::default_reference_screen()))?;
        #[cfg(not(target_arch = "wasm32"))]
        if let Some(elapsed) = super::decode_timing::finish(assembly_start) {
            self.render_timing.bridge_control_assembly = self
                .render_timing
                .bridge_control_assembly
                .saturating_add(elapsed);
        }
        #[cfg(not(target_arch = "wasm32"))]
        let spatial_start = super::decode_timing::start(self.render_timing.enabled);
        if self.object_output && !stereo {
            let base_pcm = bridge_frame.basis.base_full_band_pcm.to_vec();
            let rows = bridge_frame.basis.reconstruction_basis.rows.to_vec();
            return self.object_block(
                frame.sample_rate,
                logical_start_sample,
                &control,
                &base_pcm,
                &rows,
                calibrated_base.lfe.as_deref(),
                sample_count,
            );
        }
        let mut boundaries = vec![0_usize, sample_count];
        for event in &control.events {
            let start = usize::try_from(event.quantum.saturating_mul(32)).unwrap_or(usize::MAX);
            if start < sample_count {
                boundaries.push(start);
            }
        }
        boundaries.sort_unstable();
        boundaries.dedup();
        let zero = vec![0.0; sample_count];
        let mut coordinates = Vec::with_capacity(coordinate_count);
        for pcm in bridge_frame.basis.base_full_band_pcm {
            coordinates.push(if stereo {
                zero.as_slice()
            } else {
                pcm.as_slice()
            });
        }
        coordinates.extend(
            bridge_frame
                .basis
                .reconstruction_basis
                .rows
                .iter()
                .map(|pcm| {
                    if common_profile_stereo {
                        zero.as_slice()
                    } else {
                        pcm.as_slice()
                    }
                }),
        );
        for window in boundaries.windows(2) {
            let start = window[0];
            let end = window[1];
            if start == end {
                continue;
            }
            let event = control.events.iter().find(|event| {
                usize::try_from(event.quantum.saturating_mul(32)).ok() == Some(start)
            });
            let updates = event.map(|event| event.updates.as_slice());
            let ramp_duration = event.map_or(0, |event| u64::from(event.ramp_duration));
            let sliced = coordinates
                .iter()
                .map(|coordinate| &coordinate[start..end])
                .collect::<Vec<_>>();
            let mut outputs = active
                .iter_mut()
                .map(|channel| &mut channel[start..end])
                .collect::<Vec<_>>();
            let topology = (start == 0)
                .then_some(control.initial_topology.as_ref())
                .flatten();
            self.bridge.render_coordinates(
                &sliced,
                topology,
                updates,
                self.layout.spatial(),
                ramp_duration,
                frame.sample_rate,
                &mut outputs,
            )?;
        }
        if stereo {
            let compatibility_source = if common_profile_stereo {
                calibrated_compatibility
                    .as_ref()
                    .expect("common-profile compatibility PCM was checked")
            } else {
                &calibrated_base
            };
            add_stereo_base_downmix(&mut active, compatibility_source, self.downmix_policy)?;
        }
        #[cfg(not(target_arch = "wasm32"))]
        if let Some(elapsed) = super::decode_timing::finish(spatial_start) {
            self.render_timing.spatial_bridge_render = self
                .render_timing
                .spatial_bridge_render
                .saturating_add(elapsed);
        }
        let composition_base = if common_profile_stereo {
            calibrated_compatibility
                .as_ref()
                .expect("common-profile compatibility PCM was checked")
        } else {
            &calibrated_base
        };
        let mut channels = vec![vec![0.0; sample_count]; self.layout.channel_count()];
        let mut active_index = 0;
        for (output_index, channel) in self.layout.spatial().channels().iter().enumerate() {
            if channel.lfe {
                if let Some(lfe) = composition_base.lfe.as_deref() {
                    channels[output_index].copy_from_slice(lfe);
                }
            } else {
                channels[output_index].copy_from_slice(&active[active_index]);
                active_index += 1;
            }
        }
        self.apply_final_linked_gain(
            frame.sample_rate,
            &mut channels,
            composition_base.lfe.as_deref(),
        )?;
        self.expected_frame = self.expected_frame.saturating_add(1);
        self.expected_sample = self.expected_sample.saturating_add(sample_count as u64);
        Ok(RenderedBlock {
            sample_rate: frame.sample_rate,
            logical_start_sample,
            sample_count,
            channels,
        })
    }

    fn apply_final_linked_gain(
        &mut self,
        sample_rate: u32,
        channels: &mut [Vec<f64>],
        lfe: Option<&[f64]>,
    ) -> Result<(), OpenJocError> {
        if !self.linked_gain_enabled {
            return Ok(());
        }
        let sample_count = channels.first().map_or(0, Vec::len);
        // The public E-AC-3 adapter supplies 1536-sample frames, which are
        // split into the admitted 32-sample linked-gain blocks. Synthetic
        // short-frame renderer fixtures remain outside that adapter boundary.
        if sample_count != 1536
            && sample_count != FINAL_LINKED_GAIN_BLOCK_SAMPLES
            && sample_count != 40
        {
            return Ok(());
        }
        let active_lfe = lfe.is_some_and(|samples| !samples.is_empty());
        let active_channels = self
            .layout
            .spatial()
            .channels()
            .iter()
            .map(|channel| if channel.lfe { active_lfe } else { true })
            .collect::<Vec<_>>();
        let linked_gain = if let Some(linked_gain) = self.final_linked_gain.as_mut() {
            linked_gain
                .reconfigure(
                    sample_rate,
                    FINAL_LINKED_GAIN_BLOCK_SAMPLES,
                    &active_channels,
                )
                .map_err(|error: FinalLinkedGainError| OpenJocError::Render(error.to_string()))?;
            linked_gain
        } else {
            self.final_linked_gain = Some(
                FinalLinkedGain::new(
                    sample_rate,
                    FINAL_LINKED_GAIN_BLOCK_SAMPLES,
                    &active_channels,
                )
                .map_err(|error: FinalLinkedGainError| OpenJocError::Render(error.to_string()))?,
            );
            self.final_linked_gain
                .as_mut()
                .expect("linked gain was just initialized")
        };
        linked_gain
            .process(channels)
            .map_err(|error| OpenJocError::Render(error.to_string()))
    }

    pub(super) fn finish_with_reconstruction_tail(
        &mut self,
        tail: &ReconstructionBasis,
    ) -> Result<Vec<RenderedBlock>, OpenJocError> {
        let aligned = self.timeline.finish(tail)?;
        let mut rendered = Vec::new();
        for aligned_frame in aligned {
            let pending = self.pending_frames.pop_front().ok_or_else(|| {
                OpenJocError::Render("reconstruction timeline queue underflow".to_owned())
            })?;
            let base = DecodedAccessUnitPcm {
                sample_rate: aligned_frame.timeline.sample_rate,
                samples: u16::try_from(
                    aligned_frame.base_full_band_pcm.first().map_or(0, Vec::len),
                )
                .unwrap_or(u16::MAX),
                channel_locations: pending.channel_locations,
                channels: aligned_frame.base_full_band_pcm,
                lfe_location: pending.lfe_location,
                lfe: aligned_frame.lfe_pcm,
                downmix: pending.downmix,
                dialnorm: pending.dialnorm,
                dialnorm_segments: pending.dialnorm_segments,
            };
            let mut frame = pending.frame;
            frame.decoded.reconstruction_basis = aligned_frame.reconstruction_basis;
            rendered.push(self.render_aligned_block(
                &frame,
                &base,
                pending.compatibility_pcm.as_ref(),
                aligned_frame.timeline.logical_start_sample,
            )?);
        }
        if !self.pending_frames.is_empty() {
            return Err(OpenJocError::Render(
                "reconstruction timeline left pending frames".to_owned(),
            ));
        }
        if self.linked_gain_enabled {
            if let Some(linked_gain) = self.final_linked_gain.as_mut() {
                let sample_rate = linked_gain.sample_rate();
                let channels = linked_gain
                    .drain()
                    .map_err(|error| OpenJocError::Render(error.to_string()))?;
                rendered.push(RenderedBlock {
                    sample_rate,
                    logical_start_sample: self.expected_sample,
                    sample_count: FINAL_LINKED_GAIN_BLOCK_SAMPLES,
                    channels,
                });
            }
        }
        Ok(rendered)
    }

    pub(super) fn reset(&mut self) {
        self.bridge.reset();
        self.assembler.reset();
        self.timeline.reset();
        self.pending_frames.clear();
        self.expected_coordinates = None;
        self.next_input_frame = 0;
        self.expected_frame = 0;
        self.expected_sample = 0;
        self.base_coordinates = None;
        if let Some(linked_gain) = self.final_linked_gain.as_mut() {
            linked_gain.reset();
        }
    }
}

// Atmos Edition: object output (signals and positions of the scene's objects,
// for an external object renderer such as Windows Spatial Sound).
impl SpeakerRenderer {
    /// Maximum number of object tracks (plus one LFE track).
    const MAX_OBJECT_TRACKS: usize = 15;

    #[allow(clippy::too_many_arguments)]
    fn object_block(
        &mut self,
        sample_rate: u32,
        logical_start_sample: u64,
        control: &openjoc_scene::BridgeControlFrame,
        base_pcm: &[Vec<f64>],
        rows: &[Vec<f64>],
        lfe: Option<&[f64]>,
        sample_count: usize,
    ) -> Result<RenderedBlock, OpenJocError> {
        use openjoc_scene::{SpatialBindingRecord, SpatialSourceClass};

        let records_now: Vec<SpatialBindingRecord> =
            self.assembler.records().map(<[_]>::to_vec).unwrap_or_default();
        // Tracks are chosen once per stream: every coordinate that carries part of
        // the scene (reconstructed objects, and base channels bound to an object).
        if self.object_ordinals.is_none() {
            let ordinals = records_now
                .iter()
                .enumerate()
                .filter(|(_, record)| {
                    record.active || record.descriptor.source_class != SpatialSourceClass::ExplicitChannel
                })
                .map(|(ordinal, _)| ordinal)
                .take(Self::MAX_OBJECT_TRACKS)
                .collect::<Vec<_>>();
            self.object_ordinals = Some(ordinals);
        }
        let ordinals = self.object_ordinals.clone().unwrap_or_default();
        let base_count = base_pcm.len();
        let signal = |ordinal: usize| -> Option<&Vec<f64>> {
            if ordinal < base_count {
                base_pcm.get(ordinal)
            } else {
                rows.get(ordinal - base_count)
            }
        };

        // State at the start of this frame: the end state of the previous one.
        let mut state = self.object_state.take().unwrap_or_else(|| records_now.clone());
        let snapshot = |record: &SpatialBindingRecord, offset: usize| super::OpenJocObjectState {
            sample_offset: offset,
            coordinates: record.descriptor.coordinates.clone(),
            identity: record.descriptor.identity.clone(),
            gain: record.scalar,
            active: record.active,
        };
        let mut tracks = ordinals
            .iter()
            .map(|&ordinal| super::OpenJocObjectTrack {
                lfe: false,
                states: state.get(ordinal).map(|r| vec![snapshot(r, 0)]).unwrap_or_default(),
            })
            .collect::<Vec<_>>();

        let mut events = control.events.iter().collect::<Vec<_>>();
        events.sort_by_key(|event| event.quantum);
        for event in events {
            let offset = usize::try_from(event.quantum.saturating_mul(32)).unwrap_or(usize::MAX);
            let offset = offset.min(sample_count.saturating_sub(1));
            for update in &event.updates {
                let Some(record) = state.get_mut(update.ordinal) else { continue };
                if let Some(descriptor) = &update.descriptor {
                    if let Some(coordinates) = &descriptor.coordinates {
                        record.descriptor.coordinates.clone_from(coordinates);
                    }
                    if let Some(identity) = &descriptor.identity {
                        record.descriptor.identity.clone_from(identity);
                    }
                    if let Some(class) = &descriptor.source_class {
                        record.descriptor.source_class = class.clone();
                    }
                }
                if let Some(scalar) = update.scalar {
                    record.scalar = scalar;
                }
                if let Some(active) = update.active {
                    record.active = active;
                }
                if let Some(track) = ordinals.iter().position(|&o| o == update.ordinal) {
                    let entry = snapshot(record, offset);
                    let states = &mut tracks[track].states;
                    if states.last().is_some_and(|last| last.sample_offset == offset) {
                        states.pop();
                    }
                    states.push(entry);
                }
            }
        }
        // Keep in step with the assembler (authoritative end-of-frame state).
        self.object_state = Some(if records_now.is_empty() { state } else { records_now });

        let mut channels = ordinals
            .iter()
            .map(|&ordinal| {
                signal(ordinal)
                    .map(|pcm| pcm[..sample_count.min(pcm.len())].to_vec())
                    .unwrap_or_else(|| vec![0.0; sample_count])
            })
            .collect::<Vec<_>>();
        for channel in &mut channels {
            channel.resize(sample_count, 0.0);
        }
        let mut lfe_pcm = lfe.map(<[f64]>::to_vec).unwrap_or_else(|| vec![0.0; sample_count]);
        lfe_pcm.resize(sample_count, 0.0);
        channels.push(lfe_pcm);
        tracks.push(super::OpenJocObjectTrack { lfe: true, states: Vec::new() });

        self.object_tracks.push_back(tracks);
        self.expected_frame = self.expected_frame.saturating_add(1);
        self.expected_sample = self.expected_sample.saturating_add(sample_count as u64);
        Ok(RenderedBlock {
            sample_rate,
            logical_start_sample,
            sample_count,
            channels,
        })
    }
}
