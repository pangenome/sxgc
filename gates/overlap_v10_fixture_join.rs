// Included in the fixture copy of the banked xsa crate. Only the CLI shim is new.
fn fixture_join(a: &[String]) {
    assert_eq!(a.len(), 5); // exe, fixture-join, container, m20, docs-map
    let n = qlite::Reader::open(&a[2]).unwrap().n - 1;
    assert!(n <= 8 << 20);
    ctx::append_m20(&a[2], &std::fs::read(&a[3]).unwrap(), n);
    std::fs::remove_file(&a[3]).unwrap();
    docstarts::embed(&a[2], &docstarts::Source::DocsMap(a[4].clone()), docstarts::SAMPLE_STRIDE, None);
}
