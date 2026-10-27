# Official MAVLink C library

Source: https://github.com/mavlink/c_library_v2.git

Pinned commit: `6b4243f0e54b42ddb1e65df7a3a5e8c99d15ec1f`.

Root protocol headers are copied unchanged from that Git revision. `mavlink_registry.h` combines the upstream all/mavlink.h configuration with the all/all.h generated CRC registry, without message-specific wrappers. Regenerate with `support/update_mavlink.py`; ordinary builds need no network.

License: generated library MIT; see ../MAVLINK_COPYING.txt.
