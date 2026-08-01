"""Shared deterministic dependency-tree hashing helpers."""

import hashlib
import pathlib


def canonical_file_bytes(path):
    """Return platform-stable bytes while preserving binary files verbatim."""
    data = path.read_bytes()
    if b"\0" in data:
        return data
    try:
        data.decode("utf-8")
    except UnicodeDecodeError:
        return data
    return data.replace(b"\r\n", b"\n").replace(b"\r", b"\n")


def component_tree_digest(root, paths):
    records = []
    for relative in paths:
        candidate = root / relative
        if candidate.is_file():
            files = [candidate]
        elif candidate.is_dir():
            files = sorted(path for path in candidate.rglob("*")
                if path.is_file())
        else:
            raise FileNotFoundError(f"dependency path does not exist: {relative}")
        for path in files:
            name = path.relative_to(root).as_posix()
            digest = hashlib.sha256(canonical_file_bytes(path)).hexdigest()
            records.append((name, digest))
    records.sort()
    aggregate = hashlib.sha256()
    for name, digest in records:
        aggregate.update(name.encode("utf-8"))
        aggregate.update(b"\0")
        aggregate.update(digest.encode("ascii"))
        aggregate.update(b"\n")
    return aggregate.hexdigest(), records
