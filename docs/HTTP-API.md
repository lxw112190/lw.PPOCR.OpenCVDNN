# HTTP API

Default base URL: `http://127.0.0.1:8787`

If `api_key` is configured, add `X-API-Key: <secret>` to every OCR request.
`GET /health` and static web assets remain public for local readiness checks.

## `GET /health`

Returns service readiness, product version, backend, and whether an API Key is
required. It does not load or process an image.

## `POST /api/ocr`

Runs text detection, optional direction classification, and recognition.

```json
{"image_base64":"data:image/jpeg;base64,..."}
```

The response contains the source image size, ordered text regions, four-point
boxes, confidence scores, stage timings, and a request ID.

## `POST /api/recognize`

Recognizes already-cropped text-line images without running detection.

Single image:

```json
{"image_base64":"..."}
```

Ordered batch of 1–256 images:

```json
{"images_base64":["...","..."]}
```

## Status codes

| Status | Meaning |
| --- | --- |
| `200` | Request completed successfully |
| `400` | Invalid JSON, Base64, image, or parameter |
| `401` | Missing or invalid API Key |
| `413` | Request body exceeds `max_request_bytes` |
| `500` | Model inference or unexpected server error |

Every application-level error is JSON:

```json
{"ok":false,"request_id":"...","error":"..."}
```

The service deliberately excludes API Keys, request bodies, Base64 image data,
and recognized text from its logs.
