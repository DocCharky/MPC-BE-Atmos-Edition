//! thdatmos — décodage TrueHD Atmos en temps réel (via la bibliothèque `truehd`
//! du projet truehdd) et exposition, par une API C, des échantillons de chaque
//! élément (bed ou objet) avec sa position, son gain et son état.

use std::collections::VecDeque;
use std::ffi::{CString, c_char};
use std::fs::File;
use std::io::{BufReader, Read};
use std::path::PathBuf;

use matroska_demuxer::{Frame as MkvFrame, MatroskaFile};
use truehd::process::MAX_PRESENTATIONS;
use truehd::process::decode::{DecodedAccessUnit, Decoder};
use truehd::process::extract::Extractor;
use truehd::process::parse::Parser;
use truehd::structs::channel::ChannelLabel;
use truehd::structs::oamd::{GAIN_MINUS_INFINITY, ObjectAudioMetadataPayload, SpeakerLabels};

pub const MAX_CH: usize = 16;
pub const MAX_SAMPLES: usize = 160;

/// Nature d'un élément.
pub const KIND_OBJECT: u32 = 0; // objet (ou canal de bed positionné à son enceinte)
pub const KIND_LFE: u32 = 1; // canal de caisson de basses

#[repr(C)]
#[derive(Clone, Copy, Default, Debug)]
pub struct ThdElement {
    pub kind: u32,
    pub active: u32,
    /// Position DAMF : x gauche(-1)/droite(+1), y arrière(-1)/avant(+1), z sol(0)/plafond(1).
    pub x: f32,
    pub y: f32,
    pub z: f32,
    /// Gain linéaire.
    pub gain: f32,
}

#[repr(C)]
pub struct ThdFrame {
    /// Position du premier échantillon, en échantillons depuis le début du flux.
    pub sample_pos: u64,
    pub samples: u32,
    pub channels: u32,
    /// Fréquence d'échantillonnage (Hz).
    pub sample_rate: u32,
    /// PCM flottant [-1, 1], entrelacé : pcm[échantillon][élément].
    pub pcm: [[f32; MAX_CH]; MAX_SAMPLES],
    pub elements: [ThdElement; MAX_CH],
}

enum Source {
    Raw(BufReader<File>),
    Mkv {
        mkv: Box<MatroskaFile<BufReader<File>>>,
        track: u64,
        frame: MkvFrame,
    },
}

impl Source {
    fn open(path: PathBuf) -> Result<Self, String> {
        let is_mkv = path
            .extension()
            .map(|e| {
                let e = e.to_string_lossy().to_ascii_lowercase();
                e == "mkv" || e == "mka" || e == "mk3d"
            })
            .unwrap_or(false);
        let file = File::open(&path).map_err(|e| format!("ouverture impossible : {e}"))?;
        let reader = BufReader::with_capacity(1 << 20, file);
        if !is_mkv {
            return Ok(Source::Raw(reader));
        }
        let mkv = MatroskaFile::open(reader).map_err(|e| format!("MKV illisible : {e}"))?;
        let track = mkv
            .tracks()
            .iter()
            .find(|t| t.codec_id() == "A_TRUEHD")
            .map(|t| t.track_number().get())
            .ok_or_else(|| "aucune piste TrueHD dans ce fichier".to_string())?;
        Ok(Source::Mkv { mkv: Box::new(mkv), track, frame: MkvFrame::default() })
    }

    /// Renvoie le prochain morceau de flux TrueHD, ou None en fin de fichier.
    fn next_chunk(&mut self, buf: &mut Vec<u8>) -> Result<bool, String> {
        buf.clear();
        match self {
            Source::Raw(r) => {
                buf.resize(64 * 1024, 0);
                let n = r.read(buf).map_err(|e| e.to_string())?;
                buf.truncate(n);
                Ok(n > 0)
            }
            Source::Mkv { mkv, track, frame } => loop {
                match mkv.next_frame(frame) {
                    Ok(true) => {
                        if frame.track == *track {
                            buf.extend_from_slice(&frame.data);
                            return Ok(true);
                        }
                    }
                    Ok(false) => return Ok(false),
                    Err(e) => return Err(format!("lecture MKV : {e}")),
                }
            },
        }
    }
}

struct Update {
    pos: u64,
    index: usize,
    element: ThdElement,
}

pub struct Ctx {
    source: Option<Source>,
    mat: MatUnpacker,
    chunk: Vec<u8>,
    eof: bool,
    extractor: Extractor,
    parser: Parser,
    decoder: Decoder,
    required: [bool; MAX_PRESENTATIONS],
    total: u64,
    elements: [ThdElement; MAX_CH],
    pending: Vec<Update>,
    queue: VecDeque<Box<ThdFrame>>,
    atmos_seen: bool,
    pub last_error: Option<CString>,
}

impl Ctx {
    pub fn open(path: PathBuf) -> Result<Self, String> {
        let mut c = Self::new();
        c.source = Some(Source::open(path)?);
        Ok(c)
    }

    pub fn new() -> Self {
        let mut required = [false; MAX_PRESENTATIONS];
        required[2] = true; // 8 canaux (repli sans Atmos)
        required[3] = true; // Atmos (16 éléments)
        let mut parser = Parser::default();
        parser.set_required_presentations(&required);
        Ctx {
            source: None,
            mat: MatUnpacker::default(),
            chunk: Vec::new(),
            eof: false,
            extractor: Extractor::default(),
            parser,
            decoder: Decoder::default(),
            required,
            total: 0,
            elements: [ThdElement::default(); MAX_CH],
            pending: Vec::new(),
            queue: VecDeque::new(),
            atmos_seen: false,
            last_error: None,
        }
    }

    /// Remet le décodeur à zéro (recherche, vidage).
    pub fn reset(&mut self) {
        let required = self.required;
        let mut parser = Parser::default();
        parser.set_required_presentations(&required);
        self.parser = parser;
        self.decoder = Decoder::default();
        self.extractor = Extractor::default();
        self.mat = MatUnpacker::default();
        self.pending.clear();
        self.queue.clear();
        self.elements = [ThdElement::default(); MAX_CH];
        self.total = 0;
        self.atmos_seen = false;
    }

    /// Ajoute des octets de flux TrueHD brut.
    pub fn push(&mut self, data: &[u8]) {
        self.extractor.push_bytes(data);
        self.drain_extractor();
    }

    /// Ajoute une salve IEC 61937 (TrueHD emballé en trame MAT, telle que la
    /// produit le décodeur de MPC-BE). Renvoie false si la salve n'est pas du TrueHD.
    pub fn push_iec61937(&mut self, burst: &[u8]) -> bool {
        let mut out = Vec::with_capacity(MAT_SIZE);
        if !self.mat.unpack(burst, &mut out) {
            return false;
        }
        self.push(&out);
        true
    }

    /// Trame décodée suivante, si disponible.
    pub fn pull(&mut self) -> Option<Box<ThdFrame>> {
        self.queue.pop_front()
    }

    /// Renvoie la prochaine trame décodée (Ok(None) en fin de flux). Mode fichier.
    pub fn read(&mut self) -> Result<Option<Box<ThdFrame>>, String> {
        while self.queue.is_empty() {
            if self.eof {
                return Ok(None);
            }
            let Some(src) = self.source.as_mut() else { return Ok(None) };
            if !src.next_chunk(&mut self.chunk)? {
                self.eof = true;
                continue;
            }
            let chunk = std::mem::take(&mut self.chunk);
            self.push(&chunk);
            self.chunk = chunk;
        }
        Ok(self.queue.pop_front())
    }

    fn drain_extractor(&mut self) {
        while let Some(res) = self.extractor.next() {
            let frame = match res {
                Ok(f) => f,
                Err(truehd::utils::errors::ExtractError::InsufficientData) => break,
                Err(_) => continue, // l'extracteur se resynchronise seul
            };
            let au = match self.parser.parse(&frame) {
                Ok(au) => au,
                Err(_) => {
                    self.parser.reset_for_next_major_sync();
                    self.decoder.reset_for_next_major_sync();
                    continue;
                }
            };
            match self.decoder.decode_presentations(&au, &self.required) {
                Ok(mut res) => {
                    let d = res[3].take().or_else(|| res[2].take());
                    if let Some(d) = d {
                        self.handle(d);
                    }
                }
                Err(_) => self.decoder.reset_for_next_major_sync(),
            }
        }
    }

    fn handle(&mut self, d: DecodedAccessUnit) {
        if d.is_duplicate || d.sample_length == 0 {
            return;
        }
        for oamd in &d.oamd {
            self.atmos_seen = true;
            self.queue_oamd(oamd);
        }
        let n = d.sample_length.min(MAX_SAMPLES);
        let end = self.total + n as u64;

        // Applique les mises à jour de métadonnées échues avant la fin de cette trame.
        let mut i = 0;
        while i < self.pending.len() {
            if self.pending[i].pos < end {
                let u = self.pending.remove(i);
                self.elements[u.index] = u.element;
            } else {
                i += 1;
            }
        }

        let ch = d.channel_count.min(MAX_CH);
        if d.oamd.is_empty() && !self.atmos_seen {
            // Piste sans Atmos : chaque canal devient un objet fixe à la place de son enceinte.
            for (c, l) in d.channel_labels.iter().take(ch).enumerate() {
                self.elements[c] = channel_element(l);
            }
        }
        let mut f = Box::new(ThdFrame {
            sample_pos: self.total,
            samples: n as u32,
            channels: ch as u32,
            sample_rate: d.sampling_frequency,
            pcm: [[0.0; MAX_CH]; MAX_SAMPLES],
            elements: self.elements,
        });
        const SCALE: f32 = 1.0 / 8_388_608.0; // 2^23
        for s in 0..n {
            for c in 0..ch {
                f.pcm[s][c] = d.pcm_data[s][c] as f32 * SCALE;
            }
        }
        self.total = end;
        self.queue.push_back(f);
    }

    fn queue_oamd(&mut self, oamd: &ObjectAudioMetadataPayload) {
        let Some(oe) = &oamd.object_element else { return };
        let Some(block) = oe.md_update_info.block_update_info.first() else { return };
        let pos = self.total
            + oe.md_update_info.sample_offset as u64
            + u64::from(block.block_offset_factor_bits) * 32
            + oamd.evo_sample_offset;
        let pos_vec = oamd.get_damf_pos();
        let bed = oamd
            .program_assignment
            .bed_assignment
            .first()
            .map(|b| b.to_index_vec())
            .unwrap_or_default();

        for i in 0..oamd.object_count.min(MAX_CH) {
            let Some(od) = oe.object_data.get(i).and_then(|v| v.first()) else { continue };
            let basic = &od.object_basic_info;
            let gain = if basic.object_gain == GAIN_MINUS_INFINITY {
                0.0
            } else {
                10f32.powf(basic.object_gain as f32 / 20.0)
            };
            let mut e = ThdElement {
                kind: KIND_OBJECT,
                active: (!od.b_object_not_active) as u32,
                gain,
                ..Default::default()
            };
            if od.b_object_in_bed_or_isf {
                let label = bed.get(i).and_then(|&b| SpeakerLabels::from_u8(b as u8));
                match label {
                    Some(SpeakerLabels::LFE) | Some(SpeakerLabels::LFE2) => e.kind = KIND_LFE,
                    Some(l) => {
                        let p = l.pos();
                        (e.x, e.y, e.z) = (p[0], p[1], p[2].max(0.0));
                    }
                    None => {}
                }
            } else if let Some(p) = pos_vec.get(i).and_then(|v| v.first()) {
                (e.x, e.y, e.z) = (p[0] as f32, p[1] as f32, p[2] as f32);
            }
            self.pending.push(Update { pos, index: i, element: e });
        }
    }
}

fn channel_element(l: &ChannelLabel) -> ThdElement {
    use ChannelLabel::*;
    let (x, y, z) = match l {
        LFE | LFE2 => {
            return ThdElement { kind: KIND_LFE, active: 1, gain: 1.0, ..Default::default() };
        }
        L => (-1.0, 1.0, 0.0),
        R => (1.0, 1.0, 0.0),
        C => (0.0, 1.0, 0.0),
        Ls => (-1.0, 0.0, 0.0),
        Rs => (1.0, 0.0, 0.0),
        Lb => (-1.0, -1.0, 0.0),
        Rb => (1.0, -1.0, 0.0),
        Cb => (0.0, -1.0, 0.0),
        Lsc => (-0.5, 1.0, 0.0),
        Rsc => (0.5, 1.0, 0.0),
        Lsd => (-1.0, -0.5, 0.0),
        Rsd => (1.0, -0.5, 0.0),
        Lw => (-1.0, 0.68, 0.0),
        Rw => (1.0, 0.68, 0.0),
        Tfl => (-1.0, 1.0, 1.0),
        Tfr => (1.0, 1.0, 1.0),
        Tsl => (-1.0, 0.0, 1.0),
        Tsr => (1.0, 0.0, 1.0),
        Tbl => (-1.0, -1.0, 1.0),
        Tbr => (1.0, -1.0, 1.0),
        Tc => (0.0, 0.0, 1.0),
        Tfc => (0.0, 1.0, 1.0),
    };
    ThdElement { kind: KIND_OBJECT, active: 1, x, y, z, gain: 1.0 }
}

// ------------------------------------------------------- Déballage MAT ---

/// Taille d'une salve IEC 61937 TrueHD (en-tête 8 octets + trame MAT).
pub const MAT_SIZE: usize = 61440;
const IEC_HEADER: usize = 8;
const MAT_START_END: usize = IEC_HEADER + 20; // fin du code de début MAT
const MAT_MIDDLE: usize = 30708 + IEC_HEADER; // position du code du milieu
const MAT_MIDDLE_END: usize = MAT_MIDDLE + 12;
const MAT_LIMIT: usize = MAT_SIZE - 24; // début du code de fin
const IEC61937_TRUEHD: u8 = 0x16;

/// Reconstitue le flux TrueHD à partir des trames MAT : retire l'en-tête IEC,
/// l'inversion d'octets, les codes MAT et le bourrage entre unités d'accès.
#[derive(Default)]
struct MatUnpacker {
    carry: Vec<u8>, // données utiles en attente (unité d'accès incomplète)
}

impl MatUnpacker {
    fn unpack(&mut self, burst: &[u8], out: &mut Vec<u8>) -> bool {
        if burst.len() < MAT_LIMIT {
            return false;
        }
        // En-tête IEC 61937 : mots 16 bits petit-boutistes Pa=F872, Pb=4E1F, Pc=type.
        if burst[0] != 0x72 || burst[1] != 0xF8 || burst[2] != 0x1F || burst[3] != 0x4E || burst[4] != IEC61937_TRUEHD {
            return false;
        }
        // Charge utile inversée par paires d'octets.
        let mut swapped = vec![0u8; MAT_LIMIT];
        let mut i = IEC_HEADER;
        while i + 1 < MAT_LIMIT {
            swapped[i] = burst[i + 1];
            swapped[i + 1] = burst[i];
            i += 2;
        }
        self.carry.extend_from_slice(&swapped[MAT_START_END..MAT_MIDDLE]);
        self.carry.extend_from_slice(&swapped[MAT_MIDDLE_END..MAT_LIMIT]);

        // Découpe en unités d'accès ; les mots nuls sont du bourrage.
        let buf = &self.carry;
        let mut p = 0;
        while p + 2 <= buf.len() {
            let w = u16::from_be_bytes([buf[p], buf[p + 1]]);
            if w == 0 {
                p += 2;
                continue;
            }
            let len = ((w & 0x0FFF) as usize) * 2;
            if len < 4 {
                p += 2; // incohérent : on avance, l'extracteur se resynchronisera
                continue;
            }
            if p + len > buf.len() {
                break; // unité à cheval sur la trame MAT suivante
            }
            out.extend_from_slice(&buf[p..p + len]);
            p += len;
        }
        self.carry.drain(..p);
        if self.carry.len() > MAT_SIZE * 4 {
            self.carry.clear();
        }
        true
    }
}

// ---------------------------------------------------------------- API C ---

/// Crée un décodeur alimenté par thd_push / thd_push_iec61937.
#[unsafe(no_mangle)]
pub extern "C" fn thd_create() -> *mut Ctx {
    Box::into_raw(Box::new(Ctx::new()))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn thd_reset(ctx: *mut Ctx) {
    unsafe { &mut *ctx }.reset();
}

/// Flux TrueHD brut.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn thd_push(ctx: *mut Ctx, data: *const u8, len: usize) {
    let ctx = unsafe { &mut *ctx };
    ctx.push(unsafe { std::slice::from_raw_parts(data, len) });
}

/// Salve IEC 61937 TrueHD (trame MAT). Renvoie 1 si acceptée, 0 sinon.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn thd_push_iec61937(ctx: *mut Ctx, data: *const u8, len: usize) -> i32 {
    let ctx = unsafe { &mut *ctx };
    ctx.push_iec61937(unsafe { std::slice::from_raw_parts(data, len) }) as i32
}

/// Trame décodée suivante : 1 si `out` est rempli, 0 s'il faut plus de données.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn thd_pull(ctx: *mut Ctx, out: *mut ThdFrame) -> i32 {
    let ctx = unsafe { &mut *ctx };
    match ctx.pull() {
        Some(f) => {
            unsafe { std::ptr::copy_nonoverlapping(&*f as *const ThdFrame, out, 1) };
            1
        }
        None => 0,
    }
}


/// Ouvre un fichier .mkv/.mka (première piste TrueHD) ou un flux .thd brut.
/// `path` : chemin UTF-16 terminé par 0. Renvoie NULL en cas d'échec
/// (message dans `err`, tampon de `err_len` octets, UTF-8).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn thd_open(path: *const u16, err: *mut c_char, err_len: usize) -> *mut Ctx {
    let path = unsafe {
        let mut n = 0;
        while *path.add(n) != 0 {
            n += 1;
        }
        String::from_utf16_lossy(std::slice::from_raw_parts(path, n))
    };
    match Ctx::open(PathBuf::from(path)) {
        Ok(c) => Box::into_raw(Box::new(c)),
        Err(e) => {
            unsafe { write_err(&e, err, err_len) };
            std::ptr::null_mut()
        }
    }
}

/// Lit la trame suivante dans `out`. Renvoie 1 (trame), 0 (fin), -1 (erreur, voir thd_error).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn thd_read(ctx: *mut Ctx, out: *mut ThdFrame) -> i32 {
    let ctx = unsafe { &mut *ctx };
    match ctx.read() {
        Ok(Some(f)) => {
            unsafe { std::ptr::copy_nonoverlapping(&*f as *const ThdFrame, out, 1) };
            1
        }
        Ok(None) => 0,
        Err(e) => {
            ctx.last_error = CString::new(e).ok();
            -1
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn thd_error(ctx: *const Ctx) -> *const c_char {
    let ctx = unsafe { &*ctx };
    ctx.last_error.as_ref().map(|s| s.as_ptr()).unwrap_or(c"".as_ptr())
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn thd_close(ctx: *mut Ctx) {
    if !ctx.is_null() {
        drop(unsafe { Box::from_raw(ctx) });
    }
}

unsafe fn write_err(msg: &str, err: *mut c_char, len: usize) {
    if err.is_null() || len == 0 {
        return;
    }
    let b = msg.as_bytes();
    let n = b.len().min(len - 1);
    unsafe {
        std::ptr::copy_nonoverlapping(b.as_ptr() as *const c_char, err, n);
        *err.add(n) = 0;
    }
}
