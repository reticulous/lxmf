#pragma once
/**
 * lxmf_stamp — LXMF proof-of-work "stamp" generation and validation.
 *
 * Mirrors the reference LXMF 0.9.8 LXStamper. A stamp is a 32-byte value
 * (the PoW nonce) chosen so that SHA-256(workblock || stamp), read as a
 * big-endian 256-bit integer, is <= 2^(256 - cost). The workblock is a
 * 768 KB buffer expanded from the message_id via HKDF-SHA256 over
 * WORKBLOCK_EXPAND_ROUNDS rounds. That round count is fixed by the
 * protocol — both ends must derive the identical workblock — so it is
 * NOT a tunable.
 *
 * Cost is expressed in bits: each extra bit doubles the expected search
 * work (~2^cost SHA-256 attempts), but the one-time workblock build
 * dominates on an MCU — generation measures ~4 s on an ESP32-S3 across
 * the usual cost range. Validation is one hash over the workblock.
 */
#include <cstdint>
#include <cstddef>

constexpr int LXMF_STAMP_LEN = 32;

/* Validate a received stamp against `target_cost`. `message_id` is the
 * 32-byte SHA-256 id computed over the 4-element (unstamped) payload.
 * Returns true iff the stamp satisfies the proof of work. target_cost<=0
 * means "no stamp required" and always returns true. Returns false on a
 * workblock allocation failure (fail closed). */
bool lxmfStampValid(const uint8_t message_id[32], int target_cost,
                    const uint8_t* stamp, size_t stamp_len);

/* Generate a stamp meeting `target_cost`, writing LXMF_STAMP_LEN bytes to
 * `out_stamp`. Returns false if target_cost<=0 or the workblock could not
 * be allocated. CPU-bound for seconds; if `yield` is non-null it is
 * invoked periodically during the search so the caller can feed the task
 * watchdog and let other tasks run. */
bool lxmfStampGenerate(const uint8_t message_id[32], int target_cost,
                       uint8_t out_stamp[LXMF_STAMP_LEN], void (*yield)(void));
