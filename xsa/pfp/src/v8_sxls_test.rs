use super::*;
use std::collections::BTreeMap;
use std::sync::atomic::{AtomicUsize, Ordering};
struct Dir(PathBuf);
impl Dir {
    fn new() -> Self {
        static J: AtomicUsize = AtomicUsize::new(0);
        let d = std::env::temp_dir().join(format!(
            "v8-sxls-{}-{}",
            std::process::id(),
            J.fetch_add(1, Ordering::Relaxed)
        ));
        fs::create_dir(&d).unwrap();
        Self(d)
    }
}
impl Drop for Dir {
    fn drop(&mut self) {
        if !std::thread::panicking() {
            fs::remove_dir_all(&self.0).unwrap();
        }
    }
}
fn ok() -> io::Result<()> {
    Ok(())
}
fn path(dir: &Path, u: usize) -> PathBuf {
    dir.join(format!("band_{u}.lcp"))
}
fn expect(v: &[u64], base: usize) -> Vec<u8> {
    let mut b = b"SXLS\x01\0\0\0".to_vec();
    let esc: Vec<_> = v.iter().enumerate().filter(|(_, x)| **x >= 255).collect();
    for f in [
        v.len() as u64,
        esc.len() as u64,
        esc.iter().map(|(_, x)| **x).max().unwrap_or(0),
        base as u64,
        1,
    ] {
        b.extend(f.to_le_bytes());
    }
    b.extend(v.iter().map(|&h| h.min(255) as u8));
    for (i, &h) in esc {
        b.extend(((base + i) as u64).to_le_bytes());
        b.extend(h.to_le_bytes());
    }
    b
}
struct Compare<'a> {
    left: &'a [u8],
    calls: usize,
}
impl Write for Compare<'_> {
    fn write(&mut self, bytes: &[u8]) -> io::Result<usize> {
        assert!(!bytes.is_empty() && bytes.len() <= 65536);
        assert!(self.left.starts_with(bytes));
        self.left = &self.left[bytes.len()..];
        self.calls += 1;
        Ok(bytes.len())
    }
    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}
#[test]
fn packed_wire_and_workspace() {
    let mut p = Packed::new();
    let (v, page) = (p.v.as_ptr(), p.page.as_ptr());
    for (n, e) in [
        (1usize, 0usize),
        (3, 1),
        (4095, 4095),
        (4096, 4096),
        (4097, 4097),
        (65535, 0),
        (65536, 17),
        (65537, 65537),
        (262144, 134284),
    ] {
        let input: Vec<u64> = (0..n)
            .map(|i| {
                if i < e {
                    if i % 3 == 0 {
                        255
                    } else {
                        34067
                    }
                } else {
                    (i % 255) as u64
                }
            })
            .collect();
        for &h in &input {
            p.push(h, REAL).unwrap();
        }
        let gold = expect(&input, 700003);
        let mut cmp = Compare {
            left: &gold,
            calls: 0,
        };
        p.encode(&mut cmp, 700003).unwrap();
        assert_eq!(p.size(), gold.len() as u64);
        assert!(cmp.left.is_empty());
        assert_eq!(cmp.calls, 1 + n.div_ceil(PAGE) + (16 * e).div_ceil(PAGE));
        assert_eq!(
            (p.v.as_ptr(), p.page.as_ptr(), 2 * p.v.len() + p.page.len()),
            (v, page, 589824)
        );
        p.clear();
    }
    for h in [34068, 65535, 65536, (1u64 << 32) + 3, u64::MAX] {
        assert!(p.push(h, REAL).is_err() && p.n == 0);
    }
    let small = Shape { extent: 65, ..REAL };
    p.push(255, small).unwrap();
    assert!(p.push(0, small).is_err() && p.n == 1 && p.size() == 65);
}
fn shape(v: &[u64]) -> Shape {
    Shape {
        rows: v.len() as u64,
        escapes: v.iter().filter(|x| **x >= 255).count() as u64,
        high: *v.iter().max().unwrap() as u16,
        extent: v
            .chunks(S)
            .map(|w| 48 + w.len() + 16 * w.iter().filter(|x| **x >= 255).count())
            .max()
            .unwrap() as u64,
    }
}
#[test]
fn matches_actual_sink_wire_across_copy_unlink_reuse_and_terminal() {
    for rows in [7, 6 * S, 6 * S + 191] {
        let v: Vec<u64> = (0..rows)
            .map(|i| {
                if i % 139 == 0 {
                    34067
                } else {
                    (i % 255) as u64
                }
            })
            .collect();
        let root = Dir::new();
        let d = root.0.join("fixed");
        let old = root.0.join("legacy");
        fs::create_dir(&d).unwrap();
        fs::create_dir(&old).unwrap();
        let law = crate::laws::Laws::new(
            root.0.join("stop").display().to_string(),
            0,
            0,
            0,
            "".into(),
            "".into(),
            "".into(),
        );
        let mut writer = Some(Slices::new(&d, shape(&v)).unwrap());
        let mut held = None;
        let mut ids = BTreeMap::new();
        let mut live = std::collections::VecDeque::new();
        for (u, part) in v.chunks(S).enumerate() {
            let p = path(&d, u);
            for &h in part {
                writer.as_mut().unwrap().push(h, ok).unwrap();
            }
            let last = (u + 1) * S >= rows;
            if last {
                assert!(absent(&p).unwrap());
                writer.take().unwrap().finish(ok).unwrap();
                assert!(!d.join("slice-slots").exists());
            }
            super::super::emit_lcp_slice_mode(
                old.to_str().unwrap(),
                u as u64,
                part.len(),
                part,
                (u * S) as u64,
                &mut live,
                10,
                &law,
                true,
            );
            let b = fs::read(&p).unwrap();
            assert_eq!(b.len(), fs::metadata(path(&old, u)).unwrap().len() as usize);
            assert!(b == fs::read(path(&old, u)).unwrap() && b == expect(part, u * S));
            empty_marker(&d.join(format!("band_{u}.lcp.done"))).unwrap();
            let st = p.metadata().unwrap();
            assert_eq!(st.nlink(), if last { 1 } else { 2 });
            let id = (st.dev(), st.ino());
            assert_eq!(*ids.entry(u % 5).or_insert(id), id);
            if u == 0 {
                held = Some(fs::File::open(&p).unwrap());
            }
            if u % 5 == 0 {
                let kept = held.as_ref().unwrap().metadata().unwrap();
                assert_eq!((kept.dev(), kept.ino(), kept.len()), (id.0, id.1, st.len()));
            }
            fs::remove_file(p).unwrap();
            fs::write(d.join(format!("band_{u}.lcp.consumed")), b"").unwrap();
            for f in [path(&old, u), old.join(format!("band_{u}.lcp.done"))] {
                fs::remove_file(f).unwrap();
            }
        }
        assert!(ids.len() <= 5);
        assert_eq!(held.unwrap().metadata().unwrap().nlink(), 0); // still counted, not proof of free blocks
    }
}
#[test]
fn refuse_before_recycling_or_announcing_last_slice() {
    let rows = 6 * S;
    let sh = Shape {
        rows: rows as u64,
        escapes: 0,
        high: 1,
        extent: 48 + S as u64,
    };
    for why in [
        "early-credit",
        "extra-link",
        "bad-marker",
        "stop",
        "stale-done",
    ] {
        let d = Dir::new();
        let mut w = Slices::new(&d.0, sh).unwrap();
        for _ in 0..rows {
            w.push(1, ok).unwrap();
        }
        let p = path(&d.0, 0);
        let alias = d.0.join("slice-slots/0");
        let old = fs::read(&alias).unwrap();
        let ack = d.0.join("band_0.lcp.consumed");
        if why != "stop" {
            fs::write(ack, if why == "bad-marker" { &b"x"[..] } else { b"" }).unwrap();
        }
        if why == "extra-link" {
            fs::hard_link(&p, d.0.join("held-name")).unwrap();
        }
        if why != "early-credit" {
            fs::remove_file(&p).unwrap();
        }
        if why == "stale-done" {
            fs::write(d.0.join("band_5.lcp.done"), b"keep").unwrap();
        }
        let n = std::cell::Cell::new(0);
        let z = w.finish(|| {
            n.set(n.get() + 1);
            need(n.get() < 3, "stop")
        });
        assert!(z.is_err());
        if why == "stop" {
            assert_eq!(n.get(), 3);
        }
        assert!(
            !path(&d.0, 5).exists()
                && d.0.join("slice-slots").exists()
                && fs::read(alias).unwrap() == old
        );
        if why == "stale-done" {
            assert_eq!(fs::read(d.0.join("band_5.lcp.done")).unwrap(), b"keep");
        } else {
            assert!(!d.0.join("band_5.lcp.done").exists());
        }
    }
    for (sh, values) in [
        (
            Shape {
                rows: 1,
                escapes: 1,
                high: 255,
                extent: 65,
            },
            vec![1],
        ),
        (
            Shape {
                rows: 2,
                escapes: 0,
                high: 2,
                extent: 50,
            },
            vec![1],
        ),
        (
            Shape {
                rows: 1,
                escapes: 0,
                high: 1,
                extent: 50,
            },
            vec![1],
        ),
    ] {
        let d = Dir::new();
        let mut w = Slices::new(&d.0, sh).unwrap();
        for h in values {
            w.push(h, ok).unwrap();
        }
        assert!(w.finish(ok).is_err() && !path(&d.0, 0).exists());
    }
}
