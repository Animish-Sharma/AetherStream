import struct

from benchmarks.fetch_usgs_data import DATASET_SOURCES, DATASETS, decode_mseed2


def make_record(encoding, samples, packed_word, control_code):
    record = bytearray(512)
    record[0:6] = b"000001"
    record[6:8] = b"D "
    record[8:13] = b"ANMO "
    record[13:15] = b"00"
    record[15:18] = b"BHZ"
    record[18:20] = b"IU"
    struct.pack_into(">HHBBBBH", record, 20, 2024, 1, 0, 0, 0, 0, 0)
    struct.pack_into(">HhhBBBBiHH", record, 30, len(samples), 40, 1, 0, 0, 0, 1, 0, 64, 48)
    struct.pack_into(">HHBBBB", record, 48, 1000, 0, encoding, 1, 9, 0)
    words = [0] * 16
    words[0] = control_code << 24
    words[1] = samples[0] & 0xFFFFFFFF
    words[2] = samples[-1] & 0xFFFFFFFF
    words[3] = packed_word
    struct.pack_into(">16I", record, 64, *words)
    return bytes(record)


def test_ridgecrest_strong_motion_provenance():
    query = DATASETS["ridgecrest_strong_motion"]
    source = DATASET_SOURCES["ridgecrest_strong_motion"]
    assert (query["net"], query["sta"], query["cha"]) == ("CI", "CCC", "HNE")
    assert query["starttime"].startswith("2019-07-06T03:19")
    assert source["counts_per_mps2"] == 213979.64220881052
    assert "Ridgecrest" in source["event"]


def test_decode_steim1_record():
    samples = [100, 101, 99, 104]
    differences = [0, 1, -2, 5]
    word = sum((value & 0xFF) << (24 - 8 * index) for index, value in enumerate(differences))
    assert decode_mseed2(make_record(10, samples, word, 1)) == samples


def test_decode_steim2_record_and_concatenation():
    samples = [500, 300, 307]
    differences = [0, -200, 7]
    word = (3 << 30) | sum(
        (value & 0x3FF) << (20 - 10 * index) for index, value in enumerate(differences)
    )
    record = make_record(11, samples, word, 2)
    assert decode_mseed2(record + record) == samples + samples
