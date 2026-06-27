from __future__ import annotations

import ctypes
import os
import shutil
import sqlite3
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Iterator, Optional


FCE_OK = 0
FCE_ERR_NOT_FOUND = 4

BACKENDS = {
    "sorted_index": 1,
    "mph": 2,
    "direct_table": 3,
    "radix": 4,
    "log": 5,
}
LOOKUPS = {"exact": 1, "prefix": 2, "range": 3}
KEYS = {"bytes": 1, "u64": 2, "u128": 3, "int_tuple": 4, "sequence": 5}
VALUES = {"bytes": 1, "fixed_record": 2, "var_record": 3}
CODECS = {"none": 0, "delta_i32": 1, "rle": 2, "bitpack": 3, "user_bytes": 4, "zstd": 5}


class FceError(RuntimeError):
    pass


class _Schema(ctypes.Structure):
    _fields_ = [
        ("schema_version", ctypes.c_uint32),
        ("algorithm_version", ctypes.c_uint32),
        ("backend", ctypes.c_int),
        ("lookup", ctypes.c_int),
        ("key_kind", ctypes.c_int),
        ("value_kind", ctypes.c_int),
        ("key_codec", ctypes.c_int),
        ("value_codec", ctypes.c_int),
        ("fixed_key_size", ctypes.c_uint32),
        ("fixed_value_size", ctypes.c_uint32),
        ("direct_table_key_bits", ctypes.c_uint32),
        ("user_flags", ctypes.c_uint32),
        ("exact_key_check", ctypes.c_uint8),
        ("mmap_read", ctypes.c_uint8),
        ("read_only_after_freeze", ctypes.c_uint8),
        ("allow_duplicate_put", ctypes.c_uint8),
    ]


class _Manifest(ctypes.Structure):
    _fields_ = [
        ("magic", ctypes.c_char * 8),
        ("manifest_version", ctypes.c_uint32),
        ("schema_version", ctypes.c_uint32),
        ("algorithm_version", ctypes.c_uint32),
        ("backend_kind", ctypes.c_uint32),
        ("lookup_kind", ctypes.c_uint32),
        ("key_kind", ctypes.c_uint32),
        ("value_kind", ctypes.c_uint32),
        ("key_codec", ctypes.c_uint32),
        ("value_codec", ctypes.c_uint32),
        ("record_count", ctypes.c_uint64),
        ("index_size", ctypes.c_uint64),
        ("keys_size", ctypes.c_uint64),
        ("values_size", ctypes.c_uint64),
        ("log_size", ctypes.c_uint64),
        ("build_unix_time", ctypes.c_uint64),
        ("schema_hash", ctypes.c_uint64),
        ("index_crc64", ctypes.c_uint64),
        ("keys_crc64", ctypes.c_uint64),
        ("values_crc64", ctypes.c_uint64),
        ("log_crc64", ctypes.c_uint64),
        ("endian_tag", ctypes.c_uint64),
        ("backend_meta0", ctypes.c_uint64),
        ("backend_meta1", ctypes.c_uint64),
        ("fixed_key_size", ctypes.c_uint32),
        ("fixed_value_size", ctypes.c_uint32),
        ("direct_table_key_bits", ctypes.c_uint32),
        ("reserved0", ctypes.c_uint32),
    ]


class _PlannerInput(ctypes.Structure):
    _fields_ = [
        ("estimated_record_count", ctypes.c_uint64),
        ("dense_u64_max_key", ctypes.c_uint64),
        ("average_key_size", ctypes.c_uint32),
        ("average_value_size", ctypes.c_uint32),
        ("lookup", ctypes.c_int),
        ("key_kind", ctypes.c_int),
        ("needs_online_append", ctypes.c_uint8),
        ("prefers_prefix_scan", ctypes.c_uint8),
        ("read_mostly", ctypes.c_uint8),
        ("allow_mph", ctypes.c_uint8),
    ]


def _load_library() -> ctypes.CDLL:
    here = Path(__file__).resolve().parent
    env_library = os.environ.get("FCE_LIBRARY")
    candidates = [
        Path(env_library) if env_library else None,
        here / "fast_cache_engine.dll",
        here / "libfast_cache_engine.so",
        here / "libfast_cache_engine.dylib",
        here.parent / "build" / "Release" / "fast_cache_engine.dll",
        here.parent / "build" / "libfast_cache_engine.so",
        here.parent / "build" / "libfast_cache_engine.dylib",
        here.parent.parent / "build" / "Release" / "fast_cache_engine.dll",
        here.parent.parent / "build" / "libfast_cache_engine.so",
        here.parent.parent / "build" / "libfast_cache_engine.dylib",
    ]
    for path in candidates:
        if path and path.exists():
            return ctypes.CDLL(str(path))
    return ctypes.CDLL("fast_cache_engine")


_lib = _load_library()
_lib.fce_status_string.argtypes = [ctypes.c_int]
_lib.fce_status_string.restype = ctypes.c_char_p
_lib.fce_abi_version.restype = ctypes.c_uint32
_lib.fce_version_string.restype = ctypes.c_char_p
_lib.fce_schema_default.restype = _Schema
_lib.fce_schema_plan.argtypes = [ctypes.POINTER(_PlannerInput), ctypes.POINTER(_Schema)]
_lib.fce_builder_open.argtypes = [ctypes.c_char_p, ctypes.POINTER(_Schema), ctypes.POINTER(ctypes.c_void_p)]
_lib.fce_builder_put.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t]
_lib.fce_builder_put_u64.argtypes = [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_void_p, ctypes.c_size_t]
_lib.fce_builder_put_u128.argtypes = [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_uint64, ctypes.c_void_p, ctypes.c_size_t]
_lib.fce_builder_freeze.argtypes = [ctypes.c_void_p]
_lib.fce_builder_close.argtypes = [ctypes.c_void_p]
_lib.fce_reader_open.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p)]
_lib.fce_reader_get.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_size_t)]
_lib.fce_reader_get_u64.argtypes = [ctypes.c_void_p, ctypes.c_uint64, ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_size_t)]
_lib.fce_reader_get_u128.argtypes = [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_uint64, ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_size_t)]
_lib.fce_reader_get_batch.argtypes = [
    ctypes.c_void_p,
    ctypes.POINTER(ctypes.c_void_p),
    ctypes.POINTER(ctypes.c_size_t),
    ctypes.POINTER(ctypes.c_void_p),
    ctypes.POINTER(ctypes.c_size_t),
    ctypes.POINTER(ctypes.c_int),
    ctypes.c_size_t,
]
_lib.fce_reader_prefix_scan.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_void_p)]
_lib.fce_reader_range_scan.argtypes = [
    ctypes.c_void_p,
    ctypes.c_void_p,
    ctypes.c_size_t,
    ctypes.c_void_p,
    ctypes.c_size_t,
    ctypes.POINTER(ctypes.c_void_p),
]
_lib.fce_reader_scan_all.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]
_lib.fce_compact.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p]
_lib.fce_log_append.argtypes = [ctypes.c_char_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t]
_lib.fce_iterator_next.argtypes = [
    ctypes.c_void_p,
    ctypes.POINTER(ctypes.c_void_p),
    ctypes.POINTER(ctypes.c_size_t),
    ctypes.POINTER(ctypes.c_void_p),
    ctypes.POINTER(ctypes.c_size_t),
]
_lib.fce_iterator_close.argtypes = [ctypes.c_void_p]
_lib.fce_reader_close.argtypes = [ctypes.c_void_p]
_lib.fce_inspect.argtypes = [ctypes.c_char_p, ctypes.POINTER(_Manifest)]
_lib.fce_codec_available.argtypes = [ctypes.c_int]
_lib.fce_codec_available.restype = ctypes.c_int


def _check(status: int) -> None:
    if status != FCE_OK:
        msg = _lib.fce_status_string(status).decode("utf-8", "replace")
        raise FceError(msg)


def abi_version() -> int:
    return int(_lib.fce_abi_version())


def version_string() -> str:
    return _lib.fce_version_string().decode("utf-8", "replace")


def codec_available(codec: str) -> bool:
    return bool(_lib.fce_codec_available(CODECS[codec]))


@dataclass
class CacheSchema:
    backend: str = "sorted_index"
    lookup: str = "exact"
    key_kind: str = "bytes"
    value_kind: str = "var_record"
    key_codec: str = "none"
    value_codec: str = "none"
    mmap_read: bool = True
    exact_key_check: bool = True
    allow_duplicate_put: bool = False
    user_flags: int = 0
    fixed_key_size: int = 0
    fixed_value_size: int = 0

    def to_c(self) -> _Schema:
        s = _lib.fce_schema_default()
        s.backend = BACKENDS[self.backend]
        s.lookup = LOOKUPS[self.lookup]
        s.key_kind = KEYS[self.key_kind]
        s.value_kind = VALUES[self.value_kind]
        s.key_codec = CODECS[self.key_codec]
        s.value_codec = CODECS[self.value_codec]
        s.mmap_read = int(self.mmap_read)
        s.exact_key_check = int(self.exact_key_check)
        s.allow_duplicate_put = int(self.allow_duplicate_put)
        s.user_flags = self.user_flags
        s.fixed_key_size = self.fixed_key_size
        s.fixed_value_size = self.fixed_value_size
        return s


@dataclass
class PlannerInput:
    estimated_record_count: int = 0
    dense_u64_max_key: int = 0
    average_key_size: int = 0
    average_value_size: int = 0
    lookup: str = "exact"
    key_kind: str = "bytes"
    needs_online_append: bool = False
    prefers_prefix_scan: bool = False
    read_mostly: bool = True
    allow_mph: bool = True


def plan_schema(input: PlannerInput) -> CacheSchema:
    c_in = _PlannerInput(
        input.estimated_record_count,
        input.dense_u64_max_key,
        input.average_key_size,
        input.average_value_size,
        LOOKUPS[input.lookup],
        KEYS[input.key_kind],
        int(input.needs_online_append),
        int(input.prefers_prefix_scan),
        int(input.read_mostly),
        int(input.allow_mph),
    )
    c_out = _Schema()
    _check(_lib.fce_schema_plan(ctypes.byref(c_in), ctypes.byref(c_out)))
    reverse_backend = {v: k for k, v in BACKENDS.items()}
    reverse_lookup = {v: k for k, v in LOOKUPS.items()}
    reverse_key = {v: k for k, v in KEYS.items()}
    reverse_value = {v: k for k, v in VALUES.items()}
    reverse_codec = {v: k for k, v in CODECS.items()}
    return CacheSchema(
        backend=reverse_backend[c_out.backend],
        lookup=reverse_lookup[c_out.lookup],
        key_kind=reverse_key[c_out.key_kind],
        value_kind=reverse_value[c_out.value_kind],
        key_codec=reverse_codec[c_out.key_codec],
        value_codec=reverse_codec[c_out.value_codec],
        mmap_read=bool(c_out.mmap_read),
        exact_key_check=bool(c_out.exact_key_check),
        allow_duplicate_put=bool(c_out.allow_duplicate_put),
        user_flags=c_out.user_flags,
        fixed_key_size=c_out.fixed_key_size,
        fixed_value_size=c_out.fixed_value_size,
    )


class CacheBuilder:
    def __init__(self, path: str | os.PathLike[str], schema: CacheSchema):
        self.path = os.fsencode(path)
        self.schema = schema.to_c()
        self._ptr = ctypes.c_void_p()
        _check(_lib.fce_builder_open(self.path, ctypes.byref(self.schema), ctypes.byref(self._ptr)))

    def put(self, key: bytes, value: bytes) -> None:
        kb = ctypes.create_string_buffer(key)
        vb = ctypes.create_string_buffer(value)
        _check(_lib.fce_builder_put(self._ptr, kb, len(key), vb, len(value)))

    def put_u64(self, key: int, value: bytes) -> None:
        vb = ctypes.create_string_buffer(value)
        _check(_lib.fce_builder_put_u64(self._ptr, key, vb, len(value)))

    def put_u128(self, key_lo: int, key_hi: int, value: bytes) -> None:
        vb = ctypes.create_string_buffer(value)
        _check(_lib.fce_builder_put_u128(self._ptr, key_lo, key_hi, vb, len(value)))

    def freeze(self) -> None:
        _check(_lib.fce_builder_freeze(self._ptr))

    def close(self) -> None:
        if self._ptr:
            _check(_lib.fce_builder_close(self._ptr))
            self._ptr = ctypes.c_void_p()

    def __enter__(self) -> "CacheBuilder":
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.close()


class CacheReader:
    def __init__(self, path: str | os.PathLike[str]):
        self.path = os.fsencode(path)
        self._ptr = ctypes.c_void_p()
        _check(_lib.fce_reader_open(self.path, ctypes.byref(self._ptr)))

    def get(self, key: bytes) -> Optional[memoryview]:
        kb = ctypes.create_string_buffer(key)
        value = ctypes.c_void_p()
        value_len = ctypes.c_size_t()
        status = _lib.fce_reader_get(self._ptr, kb, len(key), ctypes.byref(value), ctypes.byref(value_len))
        if status == FCE_ERR_NOT_FOUND:
            return None
        _check(status)
        arr_t = ctypes.c_ubyte * value_len.value
        return memoryview(arr_t.from_address(value.value))

    def get_u64(self, key: int) -> Optional[memoryview]:
        value = ctypes.c_void_p()
        value_len = ctypes.c_size_t()
        status = _lib.fce_reader_get_u64(self._ptr, key, ctypes.byref(value), ctypes.byref(value_len))
        if status == FCE_ERR_NOT_FOUND:
            return None
        _check(status)
        arr_t = ctypes.c_ubyte * value_len.value
        return memoryview(arr_t.from_address(value.value))

    def get_u128(self, key_lo: int, key_hi: int) -> Optional[memoryview]:
        value = ctypes.c_void_p()
        value_len = ctypes.c_size_t()
        status = _lib.fce_reader_get_u128(
            self._ptr, key_lo, key_hi, ctypes.byref(value), ctypes.byref(value_len)
        )
        if status == FCE_ERR_NOT_FOUND:
            return None
        _check(status)
        arr_t = ctypes.c_ubyte * value_len.value
        return memoryview(arr_t.from_address(value.value))

    def get_many(self, keys: list[bytes]) -> list[Optional[memoryview]]:
        count = len(keys)
        buffers = [ctypes.create_string_buffer(k) for k in keys]
        key_ptrs = (ctypes.c_void_p * count)(*[ctypes.cast(b, ctypes.c_void_p).value for b in buffers])
        key_lens = (ctypes.c_size_t * count)(*[len(k) for k in keys])
        values = (ctypes.c_void_p * count)()
        value_lens = (ctypes.c_size_t * count)()
        statuses = (ctypes.c_int * count)()
        _check(_lib.fce_reader_get_batch(self._ptr, key_ptrs, key_lens, values, value_lens, statuses, count))
        out: list[Optional[memoryview]] = []
        for i, status in enumerate(statuses):
            if status == FCE_ERR_NOT_FOUND:
                out.append(None)
            else:
                _check(status)
                arr_t = ctypes.c_ubyte * value_lens[i]
                out.append(memoryview(arr_t.from_address(values[i])))
        return out

    def prefix_scan(self, prefix: bytes) -> "PrefixIterator":
        pb = ctypes.create_string_buffer(prefix)
        ptr = ctypes.c_void_p()
        _check(_lib.fce_reader_prefix_scan(self._ptr, pb, len(prefix), ctypes.byref(ptr)))
        return PrefixIterator(ptr)

    def range_scan(self, start_key: bytes, end_key: bytes) -> "PrefixIterator":
        sb = ctypes.create_string_buffer(start_key)
        eb = ctypes.create_string_buffer(end_key)
        ptr = ctypes.c_void_p()
        _check(_lib.fce_reader_range_scan(self._ptr, sb, len(start_key), eb, len(end_key), ctypes.byref(ptr)))
        return PrefixIterator(ptr)

    def scan_all(self) -> "PrefixIterator":
        ptr = ctypes.c_void_p()
        _check(_lib.fce_reader_scan_all(self._ptr, ctypes.byref(ptr)))
        return PrefixIterator(ptr)

    def close(self) -> None:
        if self._ptr:
            _lib.fce_reader_close(self._ptr)
            self._ptr = ctypes.c_void_p()

    def __enter__(self) -> "CacheReader":
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.close()


class PrefixIterator:
    def __init__(self, ptr: ctypes.c_void_p):
        self._ptr = ptr

    def __iter__(self) -> "PrefixIterator":
        return self

    def __next__(self) -> tuple[memoryview, memoryview]:
        if not self._ptr:
            raise StopIteration
        key = ctypes.c_void_p()
        key_len = ctypes.c_size_t()
        value = ctypes.c_void_p()
        value_len = ctypes.c_size_t()
        status = _lib.fce_iterator_next(
            self._ptr,
            ctypes.byref(key),
            ctypes.byref(key_len),
            ctypes.byref(value),
            ctypes.byref(value_len),
        )
        if status == FCE_ERR_NOT_FOUND:
            self.close()
            raise StopIteration
        _check(status)
        key_arr = ctypes.c_ubyte * key_len.value
        value_arr = ctypes.c_ubyte * value_len.value
        return memoryview(key_arr.from_address(key.value)), memoryview(value_arr.from_address(value.value))

    def close(self) -> None:
        if self._ptr:
            _lib.fce_iterator_close(self._ptr)
            self._ptr = ctypes.c_void_p()

    def __enter__(self) -> "PrefixIterator":
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.close()

    def __del__(self) -> None:
        self.close()


def inspect(path: str | os.PathLike[str]) -> dict[str, int | bytes]:
    m = _Manifest()
    _check(_lib.fce_inspect(os.fsencode(path), ctypes.byref(m)))
    return {name: getattr(m, name) for name, _ in m._fields_}


def compact(source: str | os.PathLike[str], backend: str, output: str | os.PathLike[str]) -> None:
    _check(_lib.fce_compact(os.fsencode(source), BACKENDS[backend], os.fsencode(output)))


def log_append(cache_dir: str | os.PathLike[str], key: bytes, value: bytes) -> None:
    kb = ctypes.create_string_buffer(key)
    vb = ctypes.create_string_buffer(value)
    _check(_lib.fce_log_append(os.fsencode(cache_dir), kb, len(key), vb, len(value)))


def _qident(name: str) -> str:
    if not name or "\x00" in name:
        raise ValueError("invalid sqlite identifier")
    return '"' + name.replace('"', '""') + '"'


def export_sqlite(cache_dir: str | os.PathLike[str], sqlite_path: str | os.PathLike[str], table: str = "records") -> None:
    info = inspect(cache_dir)
    source = Path(cache_dir)
    tmpdir: tempfile.TemporaryDirectory[str] | None = None
    if info["backend_kind"] == BACKENDS["log"]:
        tmpdir = tempfile.TemporaryDirectory()
        source = Path(tmpdir.name) / "compact"
        compact(cache_dir, "sorted_index", source)
    con = sqlite3.connect(sqlite_path)
    qtable = _qident(table)
    try:
        con.execute(f"CREATE TABLE IF NOT EXISTS {qtable} (key BLOB PRIMARY KEY, value BLOB NOT NULL)")
        con.execute(f"DELETE FROM {qtable}")
        with CacheReader(source) as reader:
            con.executemany(
                f"INSERT OR REPLACE INTO {qtable}(key, value) VALUES (?, ?)",
                ((bytes(k), bytes(v)) for k, v in reader.scan_all()),
            )
        con.commit()
    finally:
        con.close()
        if tmpdir is not None:
            tmpdir.cleanup()


def import_sqlite(sqlite_path: str | os.PathLike[str], cache_dir: str | os.PathLike[str], schema: CacheSchema, table: str = "records") -> None:
    con = sqlite3.connect(sqlite_path)
    qtable = _qident(table)
    try:
        rows = con.execute(f"SELECT key, value FROM {qtable} ORDER BY key").fetchall()
    finally:
        con.close()
    shutil.rmtree(cache_dir, ignore_errors=True)
    with CacheBuilder(cache_dir, schema) as builder:
        for key, value in rows:
            builder.put(bytes(key), bytes(value))
        builder.freeze()


def _load_lmdb():
    try:
        import lmdb  # type: ignore
    except ImportError as exc:
        raise ImportError("LMDB bridge requires the optional Python package 'lmdb'") from exc
    return lmdb


def export_lmdb(
    cache_dir: str | os.PathLike[str],
    lmdb_path: str | os.PathLike[str],
    map_size: int = 1 << 30,
) -> None:
    lmdb = _load_lmdb()
    info = inspect(cache_dir)
    source = Path(cache_dir)
    tmpdir: tempfile.TemporaryDirectory[str] | None = None
    if info["backend_kind"] == BACKENDS["log"]:
        tmpdir = tempfile.TemporaryDirectory()
        source = Path(tmpdir.name) / "compact"
        compact(cache_dir, "sorted_index", source)
    env = lmdb.open(os.fsdecode(lmdb_path), map_size=map_size, subdir=True, create=True, max_dbs=1)
    try:
        with env.begin(write=True) as txn:
            txn.drop(None, delete=False)
            with CacheReader(source) as reader:
                for key, value in reader.scan_all():
                    txn.put(bytes(key), bytes(value), overwrite=True)
    finally:
        env.close()
        if tmpdir is not None:
            tmpdir.cleanup()


def import_lmdb(
    lmdb_path: str | os.PathLike[str],
    cache_dir: str | os.PathLike[str],
    schema: CacheSchema,
) -> None:
    lmdb = _load_lmdb()
    env = lmdb.open(os.fsdecode(lmdb_path), readonly=True, lock=False, subdir=True, max_dbs=1)
    rows: list[tuple[bytes, bytes]] = []
    try:
        with env.begin(write=False) as txn:
            with txn.cursor() as cursor:
                rows = [(bytes(k), bytes(v)) for k, v in cursor]
    finally:
        env.close()
    rows.sort(key=lambda item: item[0])
    shutil.rmtree(cache_dir, ignore_errors=True)
    with CacheBuilder(cache_dir, schema) as builder:
        for key, value in rows:
            builder.put(key, value)
        builder.freeze()
