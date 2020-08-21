# Simple serial comms

`scomm` drives serial comms, with ymodem upload, for the TRS-20. It really only exists because the initial bootrom has a buggy YModem receiver - `scomm` transmits a 1024-byte payload to the buggy receiver in place of a usual YModem block-0 metadata payload, then aborts it, allowing a patched YModem receiver to be used for subsequent uploads.

