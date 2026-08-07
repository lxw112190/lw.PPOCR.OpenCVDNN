# C# P/Invoke example

Requires the .NET 8 SDK. The same source works on Windows and Linux; pass the
native package directory or the full native library path as the first argument.

```powershell
dotnet run --project examples/csharp -c Release -- `
  dist/local-win-x64 `
  models/ppocrv6-tiny/model.json `
  models/ppocrv6-tiny/sample.jpg `
  ocr
```

Use `recognize` instead of `ocr` for an already-cropped text-line image.
The example resolves the native library explicitly, initializes the C ABI,
prints formatted UTF-8 JSON, and releases every native allocation.

For PDF input, pass `pdf` as the operation, followed optionally by the PDF
mode (`auto`, `text`, `ocr`, or `hybrid`) and render DPI:

```powershell
dotnet run --project examples/csharp -c Release -- `
  dist/local-win-x64 `
  models/ppocrv6-tiny/model.json `
  tests/fixtures/pdf-text-sample.pdf `
  pdf auto 200
```

The result is page-level JSON. `auto` uses the PDF text layer when it is usable
and falls back to OCR for scanned pages; use `text` or `ocr` to force one path.
The Windows package must keep `pdfium.dll` beside the native OCR DLL (the
released package already has this layout).
