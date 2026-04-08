BEGIN IMMEDIATE;

-- Stores program/job-kind specific input details as INI text.
-- Example payload for seedprobe.grid:
-- [seedprobe.grid]
-- input_frame_hex=0x0000007B
ALTER TABLE exec_job
    ADD COLUMN input_ini TEXT NULL;

COMMIT;
