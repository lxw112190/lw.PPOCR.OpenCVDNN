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
