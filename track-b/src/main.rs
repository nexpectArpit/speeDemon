// Speed Demon 2026 - Track B (Index) in Rust
// Ultra-fast hybrid bitset/CSR query engine with zero third-party dependencies (D = 1.0)

use std::fmt::Write;
use std::fs::File;
use std::io::Write as IoWrite;
use std::os::unix::io::AsRawFd;
use std::thread;
use std::sync::atomic::{AtomicUsize, Ordering};

extern "C" {
    fn mmap(addr: *mut u8, len: usize, prot: i32, flags: i32, fd: i32, offset: i64) -> *mut u8;
}

const PROT_READ: i32 = 1;
const MAP_PRIVATE: i32 = 2;

struct MmapFile {
    ptr: *const u8,
    size: usize,
    _file: Option<File>,
}

unsafe impl Send for MmapFile {}
unsafe impl Sync for MmapFile {}

impl MmapFile {
    fn open(path: &str) -> Self {
        if let Ok(file) = File::open(path) {
            if let Ok(metadata) = file.metadata() {
                let size = metadata.len() as usize;
                if size > 0 {
                    let fd = file.as_raw_fd();
                    let ptr = unsafe { mmap(std::ptr::null_mut(), size, PROT_READ, MAP_PRIVATE, fd, 0) };
                    return Self {
                        ptr,
                        size,
                        _file: Some(file),
                    };
                }
            }
        }
        Self {
            ptr: std::ptr::null(),
            size: 0,
            _file: None,
        }
    }

    #[inline(always)]
    fn as_slice(&self) -> &[u8] {
        if self.ptr.is_null() || self.size == 0 {
            &[]
        } else {
            unsafe { std::slice::from_raw_parts(self.ptr, self.size) }
        }
    }
}

const CHAR_MAP: [u8; 256] = {
    let mut m = [0u8; 256];
    let mut i = 0usize;
    while i < 256 {
        let c = i as u8;
        if (c >= b'a' && c <= b'z') || (c >= b'0' && c <= b'9') {
            m[i] = c;
        } else if c >= b'A' && c <= b'Z' {
            m[i] = c + 32;
        }
        i += 1;
    }
    m
};

#[inline(always)]
fn fast_hash_swar(p: &[u8]) -> u64 {
    let len = p.len();
    let mut h = 0x517cc1b727220a95u64 ^ (len as u64).wrapping_mul(0x9e3779b97f4a7c15u64);
    let mut i = 0;
    while i + 8 <= len {
        let mut w = u64::from_le_bytes(p[i..i + 8].try_into().unwrap());
        w |= 0x2020202020202020u64;
        h = (h ^ w).wrapping_mul(0xbf58476d1ce4e5b9u64);
        i += 8;
    }
    if i < len {
        let mut rem = [0u8; 8];
        rem[..len - i].copy_from_slice(&p[i..]);
        let mut w = u64::from_le_bytes(rem);
        w |= 0x2020202020202020u64;
        let mask = if len - i == 8 { !0u64 } else { (1u64 << ((len - i) * 8)) - 1 };
        w &= mask;
        h = (h ^ w).wrapping_mul(0x94d049bb133111ebu64);
    }
    (h ^ (h >> 32)) | ((len as u64) << 56) | 1
}

#[inline(always)]
fn same_word(a: &[u8], b: &[u8]) -> bool {
    if a.len() != b.len() {
        return false;
    }
    let mut i = 0;
    while i + 8 <= a.len() {
        let x = u64::from_le_bytes(a[i..i + 8].try_into().unwrap()) | 0x2020202020202020u64;
        let y = u64::from_le_bytes(b[i..i + 8].try_into().unwrap()) | 0x2020202020202020u64;
        if x != y {
            return false;
        }
        i += 8;
    }
    while i < a.len() {
        if (a[i] | 32) != (b[i] | 32) {
            return false;
        }
        i += 1;
    }
    true
}

#[derive(Clone, Copy)]
struct DictEntry {
    h64: u64,
    id: u32,
    key: u32,
}

struct Dict {
    entries: Vec<DictEntry>,
    keys: Vec<u8>,
    count: u32,
}

impl Dict {
    fn new(cap: usize) -> Self {
        Self {
            entries: vec![DictEntry { h64: 0, id: 0, key: 0 }; cap],
            keys: Vec::new(),
            count: 0,
        }
    }

    fn grow(&mut self) {
        let new_cap = self.entries.len() * 2;
        let mut new_entries = vec![DictEntry { h64: 0, id: 0, key: 0 }; new_cap];
        let mask = (new_cap - 1) as u32;
        for e in &self.entries {
            if e.id != 0 {
                let mut x = (e.h64 as u32) & mask;
                while new_entries[x as usize].id != 0 {
                    x = (x + 1) & mask;
                }
                new_entries[x as usize] = *e;
            }
        }
        self.entries = new_entries;
    }

    #[inline(always)]
    fn insert(&mut self, s: &[u8]) -> u32 {
        let h = fast_hash_swar(s);
        let mask = (self.entries.len() - 1) as u32;
        let mut x = (h as u32) & mask;
        while self.entries[x as usize].id != 0 {
            let e = self.entries[x as usize];
            if e.h64 == h && ((e.key >> 24) as usize) == s.len() {
                let key_slice = &self.keys[(e.key & 0x00ff_ffff) as usize..((e.key & 0x00ff_ffff) as usize + s.len())];
                if same_word(key_slice, s) {
                    return e.id - 1;
                }
            }
            x = (x + 1) & mask;
        }
        if (self.count + 1) * 2 > self.entries.len() as u32 {
            self.grow();
            return self.insert(s);
        }
        let id = self.count;
        self.count += 1;
        let offset = self.keys.len() as u32;
        assert!(self.keys.len() <= 0x00ff_ffff && s.len() <= 255);
        self.keys.extend_from_slice(s);
        self.entries[x as usize] = DictEntry {
            h64: h,
            id: id + 1,
            key: offset | ((s.len() as u32) << 24),
        };
        id
    }

    #[inline(always)]
    fn find(&self, h: u64, s: &[u8]) -> u32 {
        let mask = (self.entries.len() - 1) as u32;
        let mut x = (h as u32) & mask;
        while self.entries[x as usize].id != 0 {
            let e = self.entries[x as usize];
            if e.h64 == h && ((e.key >> 24) as usize) == s.len() {
                let key_slice = &self.keys[(e.key & 0x00ff_ffff) as usize..((e.key & 0x00ff_ffff) as usize + s.len())];
                if same_word(key_slice, s) {
                    return e.id - 1;
                }
            }
            x = (x + 1) & mask;
        }
        !0u32
    }
}


#[derive(Clone, Copy)]
struct CacheEntry { key: u64, id: u32, len: u8 }
struct TokenCache { slots: Vec<CacheEntry> }
impl TokenCache {
    fn new() -> Self { Self { slots: vec![CacheEntry { key: 0, id: !0u32, len: 0 }; 4096] } }
    #[inline(always)]
    fn lookup(&mut self, dict: &Dict, token: &[u8]) -> u32 {
        if token.len() > 8 { return dict.find(fast_hash_swar(token), token); }
        let mut bytes = [0u8; 8];
        bytes[..token.len()].copy_from_slice(token);
        let mut key = u64::from_le_bytes(bytes) | 0x2020202020202020u64;
        if token.len() < 8 { key &= (1u64 << (8 * token.len())) - 1; }
        let index = (((key ^ (key >> 32)).wrapping_mul(0x9e3779b97f4a7c15)) >> 52) as usize;
        let entry = &mut self.slots[index];
        if entry.len as usize == token.len() && entry.key == key { return entry.id; }
        let id = dict.find(fast_hash_swar(token), token);
        *entry = CacheEntry { key, id, len: token.len() as u8 };
        id
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Op {
    And,
    Or,
    Not,
}

enum QueryKind {
    Bool {
        terms: Vec<u32>,
        ops: Vec<Op>,
    },
    Phrase {
        terms: Vec<u32>,
        words: Vec<Vec<u8>>,
    },
}

#[derive(Clone, Copy)]
struct DocRef {
    offset: u32,
    len: u32,
}

struct Accumulator {
    count: u64,
    sum: u64,
    first: [u32; 10],
    nfirst: usize,
}

impl Accumulator {
    #[inline(always)]
    fn new() -> Self {
        Self {
            count: 0,
            sum: 0,
            first: [0; 10],
            nfirst: 0,
        }
    }

    #[inline(always)]
    fn add(&mut self, doc: u32) {
        self.count += 1;
        self.sum += doc as u64;
        if self.nfirst < 10 {
            self.first[self.nfirst] = doc;
            self.nfirst += 1;
        }
    }
}

fn main() {
    let t_start = std::time::Instant::now();
    let args: Vec<String> = std::env::args().collect();
    let data_dir = if args.len() >= 2 { &args[1] } else { "/data" };
    let out_dir = if args.len() >= 3 { &args[2] } else { "/out" };

    let mut num_threads = 12usize;
    if let Ok(val) = std::env::var("SD_THREADS") {
        if let Ok(n) = val.parse::<usize>() {
            if (1..=16).contains(&n) {
                num_threads = n;
            }
        }
    }

    let qf = MmapFile::open(&format!("{}/queries.txt", data_dir));
    let q_bytes = qf.as_slice();

    let mut dict = Dict::new(262144);
    let mut queries = Vec::with_capacity(50000);

    let mut pos = 0;
    while pos < q_bytes.len() {
        while pos < q_bytes.len() && (q_bytes[pos] == b'\n' || q_bytes[pos] == b'\r') {
            pos += 1;
        }
        if pos >= q_bytes.len() {
            break;
        }
        let start = pos;
        while pos < q_bytes.len() && q_bytes[pos] != b'\n' && q_bytes[pos] != b'\r' {
            pos += 1;
        }
        let line = &q_bytes[start..pos];

        if line.starts_with(b"BOOL ") {
            let mut terms = Vec::new();
            let mut ops = Vec::new();
            let mut p = 5;
            let mut is_op = false;
            while p < line.len() {
                let sp = line[p..].iter().position(|&b| b == b' ').map(|i| p + i).unwrap_or(line.len());
                let word = &line[p..sp];
                if is_op {
                    match word {
                        b"AND" => ops.push(Op::And),
                        b"OR" => ops.push(Op::Or),
                        b"NOT" => ops.push(Op::Not),
                        _ => {}
                    }
                    is_op = false;
                } else {
                    terms.push(dict.insert(word));
                    is_op = true;
                }
                p = sp + 1;
            }
            queries.push(QueryKind::Bool { terms, ops });
        } else if line.starts_with(b"PHRASE \"") {
            let mut terms = Vec::new();
            let mut words = Vec::new();
            let mut p = 8;
            let end_quote = line[8..].iter().rposition(|&b| b == b'"').map(|i| 8 + i).unwrap_or(line.len());
            while p < end_quote {
                let sp = line[p..end_quote].iter().position(|&b| b == b' ').map(|i| p + i).unwrap_or(end_quote);
                let word = &line[p..sp];
                terms.push(dict.insert(word));
                words.push(word.to_ascii_lowercase());
                p = sp + 1;
            }
            queries.push(QueryKind::Phrase { terms, words });
        }
    }

    let num_terms = dict.count as usize;
    eprintln!("[PERF] Parse queries: {:?}", t_start.elapsed());
    let t_queries_done = std::time::Instant::now();

    let df = MmapFile::open(&format!("{}/docs.txt", data_dir));
    let doc_bytes = df.as_slice();

    // Determine number of documents N
    let mut last = doc_bytes.len();
    while last > 0 && doc_bytes[last - 1] == b'\n' {
        last -= 1;
    }
    while last > 0 && doc_bytes[last - 1] != b'\n' {
        last -= 1;
    }
    let mut num_docs = 0u32;
    let mut p = last;
    while p < doc_bytes.len() && doc_bytes[p] >= b'0' && doc_bytes[p] <= b'9' {
        num_docs = num_docs * 10 + ((doc_bytes[p] - b'0') as u32);
        p += 1;
    }
    num_docs += 1;
    let nd = num_docs as usize;
    let num_words = (nd + 63) / 64;

    let w_count = num_threads.min(nd);

    // Compute document boundaries aligned to 64
    let mut bounds = vec![0usize; w_count + 1];
    bounds[w_count] = doc_bytes.len();
    for i in 1..w_count {
        let mut target = doc_bytes.len() * i / w_count;
        while target > 0 && target < doc_bytes.len() && doc_bytes[target - 1] != b'\n' {
            target += 1;
        }
        while target < doc_bytes.len() {
            let mut check_id = 0u32;
            let mut cp = target;
            while cp < doc_bytes.len() && doc_bytes[cp] >= b'0' && doc_bytes[cp] <= b'9' {
                check_id = check_id * 10 + ((doc_bytes[cp] - b'0') as u32);
                cp += 1;
            }
            if (check_id & 63) == 0 {
                break;
            }
            let nl = doc_bytes[target..].iter().position(|&b| b == b'\n').map(|idx| target + idx);
            target = nl.map(|idx| idx + 1).unwrap_or(doc_bytes.len());
        }
        bounds[i] = target;
    }

    // Pass 1: Parse doc refs and compute document frequencies per worker
    let mut worker_df = vec![vec![0u32; num_terms]; w_count];
    let mut doc_refs_vec = vec![DocRef { offset: 0, len: 0 }; nd];
    let doc_refs_addr = doc_refs_vec.as_mut_ptr() as usize;
    let dict_ref = &dict;

    thread::scope(|s| {
        for (_wid, (wdf, chunk_range)) in worker_df.iter_mut().zip(bounds.windows(2)).enumerate() {
            s.spawn(move || {
                let d_ptr = doc_refs_addr as *mut DocRef;
                let start_idx = chunk_range[0];
                let end_idx = chunk_range[1];
                let mut sc = start_idx;
                let mut last_doc = vec![!0u32; num_terms];
                let mut token_cache = TokenCache::new();

                while sc < end_idx {
                    let mut doc_id = 0u32;
                    while sc < end_idx && doc_bytes[sc] >= b'0' && doc_bytes[sc] <= b'9' {
                        doc_id = doc_id * 10 + ((doc_bytes[sc] - b'0') as u32);
                        sc += 1;
                    }
                    if sc < end_idx && doc_bytes[sc] == b'\t' {
                        sc += 1;
                    }
                    let text_start = sc;
                    let text_end = doc_bytes[sc..end_idx]
                        .iter()
                        .position(|&b| b == b'\n')
                        .map(|i| sc + i)
                        .unwrap_or(end_idx);

                    unsafe {
                        *d_ptr.add(doc_id as usize) = DocRef {
                            offset: text_start as u32,
                            len: (text_end - text_start) as u32,
                        };
                    }

                    sc = if text_end < end_idx { text_end + 1 } else { end_idx };

                    // Tokenize document
                    let text = &doc_bytes[text_start..text_end];
                    let mut tp = 0;
                    while tp < text.len() {
                        while tp < text.len() && CHAR_MAP[text[tp] as usize] == 0 {
                            tp += 1;
                        }
                        if tp >= text.len() {
                            break;
                        }
                        let tok_start = tp;
                        while tp < text.len() && CHAR_MAP[text[tp] as usize] != 0 {
                            tp += 1;
                        }
                        let tok_len = tp - tok_start;
                        if tok_len <= 32 {
                            let tok = &text[tok_start..tp];
                            let tid = token_cache.lookup(dict_ref, tok);
                            if tid != !0u32 && last_doc[tid as usize] != doc_id {
                                last_doc[tid as usize] = doc_id;
                                wdf[tid as usize] += 1;
                            }
                        }
                    }
                }
            });
        }
    });
    eprintln!("[PERF] Pass 1 (scan docs & freq): {:?}", t_queries_done.elapsed());
    let t_pass1_done = std::time::Instant::now();

    // Sum document frequencies and identify dense terms
    let mut total_df = vec![0u32; num_terms];
    for wdf in &worker_df {
        for (t, &cnt) in wdf.iter().enumerate() {
            total_df[t] += cnt;
        }
    }

    const DENSE_THRESHOLD: u32 = 4096;
    let mut bitset_map = vec![!0u32; num_terms];
    let mut num_dense = 0u32;

    let mut sparse_offsets = vec![vec![0u32; num_terms]; w_count];
    let mut total_array_postings = 0u32;

    for t in 0..num_terms {
        if total_df[t] >= DENSE_THRESHOLD {
            bitset_map[t] = num_dense;
            num_dense += 1;
        } else {
            for wid in 0..w_count {
                sparse_offsets[wid][t] = total_array_postings;
                total_array_postings += worker_df[wid][t];
            }
        }
    }

    drop(worker_df);

    // Flat 1D bitset buffer: (num_dense * num_words) contiguous u64 words
    let mut dense_bitsets = vec![0u64; (num_dense as usize) * num_words];
    let dense_bitsets_addr = dense_bitsets.as_mut_ptr() as usize;

    let mut all_postings = vec![0u32; total_array_postings as usize];
    let all_postings_addr = all_postings.as_mut_ptr() as usize;

    let bitset_map_ref = &bitset_map;

    // Pass 2: Fill dense bitsets and sparse postings
    thread::scope(|s| {
        for (_wid, (mut l_offsets, chunk_range)) in sparse_offsets.into_iter().zip(bounds.windows(2)).enumerate() {
            s.spawn(move || {
                let post_ptr = all_postings_addr as *mut u32;
                let bitset_ptr = dense_bitsets_addr as *mut u64;
                let start_idx = chunk_range[0];
                let end_idx = chunk_range[1];
                let mut sc = start_idx;
                let mut last_doc = vec![!0u32; num_terms];
                let mut token_cache = TokenCache::new();

                while sc < end_idx {
                    let mut doc_id = 0u32;
                    while sc < end_idx && doc_bytes[sc] >= b'0' && doc_bytes[sc] <= b'9' {
                        doc_id = doc_id * 10 + ((doc_bytes[sc] - b'0') as u32);
                        sc += 1;
                    }
                    if sc < end_idx && doc_bytes[sc] == b'\t' {
                        sc += 1;
                    }
                    let text_start = sc;
                    let text_end = doc_bytes[sc..end_idx]
                        .iter()
                        .position(|&b| b == b'\n')
                        .map(|i| sc + i)
                        .unwrap_or(end_idx);

                    sc = if text_end < end_idx { text_end + 1 } else { end_idx };

                    let text = &doc_bytes[text_start..text_end];
                    let mut tp = 0;
                    while tp < text.len() {
                        while tp < text.len() && CHAR_MAP[text[tp] as usize] == 0 {
                            tp += 1;
                        }
                        if tp >= text.len() {
                            break;
                        }
                        let tok_start = tp;
                        while tp < text.len() && CHAR_MAP[text[tp] as usize] != 0 {
                            tp += 1;
                        }
                        let tok_len = tp - tok_start;
                        if tok_len <= 32 {
                            let tok = &text[tok_start..tp];
                            let tid = token_cache.lookup(dict_ref, tok);
                            if tid != !0u32 && last_doc[tid as usize] != doc_id {
                                last_doc[tid as usize] = doc_id;
                                let bs_id = bitset_map_ref[tid as usize];
                                if bs_id != !0u32 {
                                    // Range is 64-aligned, exclusive single writer per word!
                                    unsafe {
                                        let offset = (bs_id as usize) * num_words + ((doc_id >> 6) as usize);
                                        *bitset_ptr.add(offset) |= 1u64 << (doc_id & 63);
                                    }
                                } else {
                                    unsafe {
                                        let write_pos = l_offsets[tid as usize];
                                        l_offsets[tid as usize] += 1;
                                        *post_ptr.add(write_pos as usize) = doc_id;
                                    }
                                }
                            }
                        }
                    }
                }
            });
        }
    });
    eprintln!("[PERF] Pass 2 (fill bitsets & postings): {:?}", t_pass1_done.elapsed());
    let t_pass2_done = std::time::Instant::now();

    let doc_refs = doc_refs_vec;

    let phrase_matches = |doc_id: u32, words: &[Vec<u8>]| -> bool {
        let doc = doc_refs[doc_id as usize];
        let text = &doc_bytes[doc.offset as usize..(doc.offset + doc.len) as usize];
        let mut tp = 0;
        while tp < text.len() {
            while tp < text.len() && CHAR_MAP[text[tp] as usize] == 0 {
                tp += 1;
            }
            let tok_start = tp;
            while tp < text.len() && CHAR_MAP[text[tp] as usize] != 0 {
                tp += 1;
            }
            let tok = &text[tok_start..tp];
            if !same_word(tok, &words[0]) {
                continue;
            }
            let mut next_tp = tp;
            let mut matched = true;
            for word in &words[1..] {
                while next_tp < text.len() && CHAR_MAP[text[next_tp] as usize] == 0 {
                    next_tp += 1;
                }
                let next_start = next_tp;
                while next_tp < text.len() && CHAR_MAP[text[next_tp] as usize] != 0 {
                    next_tp += 1;
                }
                let next_tok = &text[next_start..next_tp];
                if !same_word(next_tok, word) {
                    matched = false;
                    break;
                }
            }
            if matched {
                return true;
            }
        }
        false
    };

    let num_queries = queries.len();
    let mut results = vec![String::new(); num_queries];
    let mut candidates: Vec<Vec<u32>> = vec![Vec::new(); num_queries];
    const CANDIDATE_LIMIT: usize = 1 << 20;
    let candidate_count = AtomicUsize::new(0);
    let candidate_count_ref = &candidate_count;

    // Compute sparse posting list slices (start, len)
    let mut term_slices: Vec<(u32, u32)> = vec![(0, 0); num_terms];
    let mut current_offset = 0u32;
    for t in 0..num_terms {
        if bitset_map[t] == !0u32 {
            let count = total_df[t];
            if count > 0 {
                term_slices[t] = (current_offset, count);
                current_offset += count;
            }
        }
    }

    let postings_slice = &all_postings;
    let dense_slice = &dense_bitsets;
    let queries_ref = &queries;
    let total_df_ref = &total_df;
    let bitset_map_ref = &bitset_map;
    let term_slices_ref = &term_slices;

    // Partition queries across threads: safe, zero-unsafe chunking
    let chunk_size = ((num_queries + num_threads - 1) / num_threads).max(1);

    thread::scope(|s| {
        for (wid, (res_chunk, candidate_chunk)) in results.chunks_mut(chunk_size).zip(candidates.chunks_mut(chunk_size)).enumerate() {
            let start_q = wid * chunk_size;
            s.spawn(move || {
                let mut cb = vec![0u64; num_words];
                let mut cur = Vec::with_capacity(4096);
                let mut nxt = Vec::with_capacity(4096);

                for (local_idx, out_line) in res_chunk.iter_mut().enumerate() {
                    let q_idx = start_q + local_idx;
                    let query = &queries_ref[q_idx];
                    let mut acc = Accumulator::new();

                    match query {
                        QueryKind::Phrase { terms, words } => {
                            let mut order_storage = [0usize; 8];
                            for (i, slot) in order_storage[..terms.len()].iter_mut().enumerate() { *slot = i; }
                            let order = &mut order_storage[..terms.len()];
                            let mut absent = false;
                            for &t in terms {
                                if total_df_ref[t as usize] == 0 {
                                    absent = true;
                                    break;
                                }
                            }

                            if !absent {
                                order.sort_by_key(|&i| total_df_ref[terms[i] as usize]);
                                let first_t = terms[order[0]] as usize;
                                cur.clear();

                                if bitset_map_ref[first_t] != !0u32 {
                                    let bs_id = bitset_map_ref[first_t] as usize;
                                    let bitset = &dense_slice[bs_id * num_words..(bs_id + 1) * num_words];
                                    for (w, &mask) in bitset.iter().enumerate() {
                                        let mut m = mask;
                                        while m != 0 {
                                            cur.push((w * 64 + m.trailing_zeros() as usize) as u32);
                                            m &= m - 1;
                                        }
                                    }
                                } else {
                                    let (off, len) = term_slices_ref[first_t];
                                    if len > 0 {
                                        cur.extend_from_slice(&postings_slice[off as usize..(off + len) as usize]);
                                    }
                                }

                                for &idx in &order[1..] {
                                    if cur.is_empty() {
                                        break;
                                    }
                                    let t = terms[idx] as usize;
                                    if total_df_ref[t] == 0 {
                                        cur.clear();
                                        break;
                                    }
                                    if bitset_map_ref[t] != !0u32 {
                                        let bs_id = bitset_map_ref[t] as usize;
                                        let bits = &dense_slice[bs_id * num_words..(bs_id + 1) * num_words];
                                        cur.retain(|&doc| ((bits[(doc >> 6) as usize] >> (doc & 63)) & 1) != 0);
                                    } else {
                                        let (off, len) = term_slices_ref[t];
                                        let slice = &postings_slice[off as usize..(off + len) as usize];
                                        let mut cursor = 0;
                                        cur.retain(|&doc| {
                                            cursor += slice[cursor..].partition_point(|&value| value < doc);
                                            cursor < slice.len() && slice[cursor] == doc
                                        });
                                    }
                                }

                                let reserved = candidate_count_ref.fetch_add(cur.len(), Ordering::Relaxed);
                                if reserved.saturating_add(cur.len()) <= CANDIDATE_LIMIT {
                                    candidate_chunk[local_idx].extend_from_slice(&cur);
                                } else {
                                    candidate_count_ref.fetch_sub(cur.len(), Ordering::Relaxed);
                                    for &doc in &cur {
                                        if phrase_matches(doc, words) { acc.add(doc); }
                                    }
                                }
                            }
                        }

                        QueryKind::Bool { terms, ops } => {
                            let first_t = terms[0] as usize;
                            let mut is_bitmap = bitset_map_ref[first_t] != !0u32;
                            cur.clear();

                            if is_bitmap {
                                let bs_id = bitset_map_ref[first_t] as usize;
                                cb.copy_from_slice(&dense_slice[bs_id * num_words..(bs_id + 1) * num_words]);
                            } else {
                                let (off, len) = term_slices_ref[first_t];
                                if len > 0 {
                                    cur.extend_from_slice(&postings_slice[off as usize..(off + len) as usize]);
                                }
                            }

                            for (&op, &t_id) in ops.iter().zip(&terms[1..]) {
                                let t = t_id as usize;
                                let op_is_dense = bitset_map_ref[t] != !0u32;

                                if op_is_dense {
                                    let bs_id = bitset_map_ref[t] as usize;
                                    let rhs = &dense_slice[bs_id * num_words..(bs_id + 1) * num_words];
                                    if is_bitmap {
                                        match op {
                                            Op::And => {
                                                for (d, s) in cb.iter_mut().zip(rhs) {
                                                    *d &= *s;
                                                }
                                            }
                                            Op::Or => {
                                                for (d, s) in cb.iter_mut().zip(rhs) {
                                                    *d |= *s;
                                                }
                                            }
                                            Op::Not => {
                                                for (d, s) in cb.iter_mut().zip(rhs) {
                                                    *d &= !*s;
                                                }
                                            }
                                        }
                                    } else if op == Op::Or {
                                        cb.copy_from_slice(rhs);
                                        for &doc in &cur {
                                            cb[(doc >> 6) as usize] |= 1u64 << (doc & 63);
                                        }
                                        is_bitmap = true;
                                    } else {
                                        match op {
                                            Op::And => cur.retain(|&doc| ((rhs[(doc >> 6) as usize] >> (doc & 63)) & 1) != 0),
                                            Op::Not => cur.retain(|&doc| ((rhs[(doc >> 6) as usize] >> (doc & 63)) & 1) == 0),
                                            _ => {}
                                        }
                                    }
                                } else {
                                    let (off, len) = term_slices_ref[t];
                                    let sparse_slice = if len > 0 {
                                        &postings_slice[off as usize..(off + len) as usize]
                                    } else {
                                        &[]
                                    };

                                    if !is_bitmap && op == Op::Or && (cur.len() + sparse_slice.len() > DENSE_THRESHOLD as usize) {
                                        cb.fill(0);
                                        for &doc in &cur {
                                            cb[(doc >> 6) as usize] |= 1u64 << (doc & 63);
                                        }
                                        is_bitmap = true;
                                    }

                                    if is_bitmap {
                                        match op {
                                            Op::And => {
                                                cur.clear();
                                                for &doc in sparse_slice {
                                                    if ((cb[(doc >> 6) as usize] >> (doc & 63)) & 1) != 0 {
                                                        cur.push(doc);
                                                    }
                                                }
                                                is_bitmap = false;
                                            }
                                            Op::Or => {
                                                for &doc in sparse_slice {
                                                    cb[(doc >> 6) as usize] |= 1u64 << (doc & 63);
                                                }
                                            }
                                            Op::Not => {
                                                for &doc in sparse_slice {
                                                    cb[(doc >> 6) as usize] &= !(1u64 << (doc & 63));
                                                }
                                            }
                                        }
                                    } else {
                                        nxt.clear();
                                        let mut a = 0;
                                        let mut b = 0;
                                        match op {
                                            Op::And => {
                                                while a < cur.len() && b < sparse_slice.len() {
                                                    if cur[a] == sparse_slice[b] {
                                                        nxt.push(cur[a]);
                                                        a += 1;
                                                        b += 1;
                                                    } else if cur[a] < sparse_slice[b] {
                                                        a += 1;
                                                    } else {
                                                        b += 1;
                                                    }
                                                }
                                            }
                                            Op::Or => {
                                                while a < cur.len() && b < sparse_slice.len() {
                                                    if cur[a] == sparse_slice[b] {
                                                        nxt.push(cur[a]);
                                                        a += 1;
                                                        b += 1;
                                                    } else if cur[a] < sparse_slice[b] {
                                                        nxt.push(cur[a]);
                                                        a += 1;
                                                    } else {
                                                        nxt.push(sparse_slice[b]);
                                                        b += 1;
                                                    }
                                                }
                                                nxt.extend_from_slice(&cur[a..]);
                                                nxt.extend_from_slice(&sparse_slice[b..]);
                                            }
                                            Op::Not => {
                                                while a < cur.len() && b < sparse_slice.len() {
                                                    if cur[a] == sparse_slice[b] {
                                                        a += 1;
                                                        b += 1;
                                                    } else if cur[a] < sparse_slice[b] {
                                                        nxt.push(cur[a]);
                                                        a += 1;
                                                    } else {
                                                        b += 1;
                                                    }
                                                }
                                                nxt.extend_from_slice(&cur[a..]);
                                            }
                                        }
                                        std::mem::swap(&mut cur, &mut nxt);
                                    }
                                }
                            }

                            if is_bitmap {
                                for (w, &mask) in cb.iter().enumerate() {
                                    if mask == 0 {
                                        continue;
                                    }
                                    let base = (w * 64) as u32;
                                    let bits = mask.count_ones();
                                    if bits <= 8 {
                                        let mut m = mask;
                                        while m != 0 {
                                            acc.add(base + m.trailing_zeros());
                                            m &= m - 1;
                                        }
                                    } else {
                                        acc.count += bits as u64;
                                        acc.sum += (base as u64) * (bits as u64)
                                            + (mask & 0xaaaaaaaaaaaaaaaau64).count_ones() as u64
                                            + 2 * (mask & 0xccccccccccccccccu64).count_ones() as u64
                                            + 4 * (mask & 0xf0f0f0f0f0f0f0f0u64).count_ones() as u64
                                            + 8 * (mask & 0xff00ff00ff00ff00u64).count_ones() as u64
                                            + 16 * (mask & 0xffff0000ffff0000u64).count_ones() as u64
                                            + 32 * (mask & 0xffffffff00000000u64).count_ones() as u64;
                                        let mut m = mask;
                                        while m != 0 && acc.nfirst < 10 {
                                            acc.first[acc.nfirst] = base + m.trailing_zeros();
                                            acc.nfirst += 1;
                                            m &= m - 1;
                                        }
                                    }
                                }
                            } else {
                                for &doc in &cur {
                                    acc.add(doc);
                                }
                            }
                        }
                    }

                    // Format output line
                    let mut line_buf = String::with_capacity(48);
                    write!(&mut line_buf, "{} {}", acc.count, acc.sum).unwrap();
                    for &id in &acc.first[..acc.nfirst] {
                        write!(&mut line_buf, " {}", id).unwrap();
                    }
                    line_buf.push('\n');
                    *out_line = line_buf;
                }
            });
        }
    });



    // Group bounded phrase candidates by document; tokenize each selected document once.
    let total_candidates: usize = candidates.iter().map(Vec::len).sum();
    let mut pairs = Vec::with_capacity(total_candidates);
    for (query, docs) in candidates.into_iter().enumerate() {
        for doc in docs { pairs.push(((doc as u64) << 32) | query as u64); }
    }
    pairs.sort_unstable();
    let mut group_bounds = vec![0usize; num_threads + 1];
    group_bounds[num_threads] = pairs.len();
    for (i, bound) in group_bounds.iter_mut().enumerate().take(num_threads).skip(1) {
        let mut pos = pairs.len() * i / num_threads;
        while pos > 0 && pos < pairs.len() && (pairs[pos] >> 32) == (pairs[pos - 1] >> 32) { pos += 1; }
        *bound = pos;
    }
    let pairs_ref = &pairs;
    let docs_ref = &doc_refs;
    let local_matches: Vec<Vec<u64>> = thread::scope(|scope| {
        let mut handles = Vec::new();
        for range in group_bounds.windows(2) {
            if range[0] == range[1] { continue; }
            let begin = range[0]; let end = range[1];
            handles.push(scope.spawn(move || {
                let mut found = Vec::new();
                let mut tokens: Vec<(usize, usize)> = Vec::with_capacity(256);
                let mut pos = begin;
                while pos < end {
                    let doc_id = (pairs_ref[pos] >> 32) as u32;
                    let mut next = pos + 1;
                    while next < end && (pairs_ref[next] >> 32) as u32 == doc_id { next += 1; }
                    let doc = docs_ref[doc_id as usize];
                    let text = &doc_bytes[doc.offset as usize..(doc.offset as usize + doc.len as usize)];
                    tokens.clear();
                    let mut at = 0;
                    while at < text.len() {
                        while at < text.len() && CHAR_MAP[text[at] as usize] == 0 { at += 1; }
                        if at == text.len() { break; }
                        let start = at;
                        while at < text.len() && CHAR_MAP[text[at] as usize] != 0 { at += 1; }
                        tokens.push((start, at));
                    }
                    for pair in &pairs_ref[pos..next] {
                        let query_id = *pair as u32;
                        if let QueryKind::Phrase { words, .. } = &queries_ref[query_id as usize] {
                            if tokens.len() < words.len() { continue; }
                            for start in 0..=tokens.len() - words.len() {
                                let mut matches = true;
                                for (i, word) in words.iter().enumerate() {
                                    let (a, b) = tokens[start + i];
                                    if !same_word(&text[a..b], word) { matches = false; break; }
                                }
                                if matches { found.push(((query_id as u64) << 32) | doc_id as u64); break; }
                            }
                        }
                    }
                    pos = next;
                }
                found
            }));
        }
        handles.into_iter().map(|h| h.join().unwrap()).collect()
    });
    drop(pairs);
    let mut matches = Vec::with_capacity(local_matches.iter().map(Vec::len).sum::<usize>());
    for local in local_matches { matches.extend(local); }
    matches.sort_unstable();
    let mut pos = 0;
    while pos < matches.len() {
        let query = (matches[pos] >> 32) as usize;
        let mut acc = Accumulator::new();
        while pos < matches.len() && (matches[pos] >> 32) as usize == query {
            acc.add(matches[pos] as u32); pos += 1;
        }
        let out = &mut results[query];
        out.clear();
        write!(out, "{} {}", acc.count, acc.sum).unwrap();
        for id in &acc.first[..acc.nfirst] { write!(out, " {}", id).unwrap(); }
        out.push('\n');
    }
    drop(matches);

    eprintln!("[PERF] Queries execution: {:?}", t_pass2_done.elapsed());
    let t_queries_eval_done = std::time::Instant::now();

    // Write results to results.txt
    let out_path = format!("{}/results.txt", out_dir);
    if let Ok(file) = File::create(&out_path) {
        let mut writer = std::io::BufWriter::with_capacity(1024 * 1024, file);
        for res_line in &results {
            let _ = writer.write_all(res_line.as_bytes());
        }
        let _ = writer.flush();
    }
    std::process::exit(0);
}
