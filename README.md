# About
This is an adapted version of the gstreamer rfbsrc plugin using libvncclient for RFB protocol abstraction instead of a custom decoder as in the gstreamer plugin.  
This not only leads to better performance, but also enables support of more encodings and implementation of user input path, which was already included with this adaption.

# Building
To build this plugin, ready to replace the original rfbsrc library, execute the following command on a machine with the target arch. _NOTE_: You may require to install other dependencies (gstreamer libs, compiler, ...).

``` bash
mkdir -p ./build/
gcc -c -fPIC ./gstrfbsrc.c \
      -o ./build/gstrfbsrc.o \
      -DVERSION=\"1.0.0\" -DPACKAGE=\"gst-libvncclient-rfbsrc\" \
      -DGST_LICENSE=\"LGPL\" \
      -DGST_PACKAGE_NAME=\"gst-libvncclient-rfbsrc\" \
      -DGST_PACKAGE_ORIGIN=\"https://github.com/jgrasboeck/gst-libvncclient-rfbsrc\" \
      $(pkg-config --cflags gstreamer-1.0 gstreamer-base-1.0 gstreamer-video-1.0) &&
gcc -o ./build/libgstrfbsrc.o \
      -shared /tmp/gst-libvncclient-rfbsrc/build/gstrfbsrc.o \
      -Wl,--as-needed -Wl,--no-undefined -fPIC \
      -Wl,--start-group -lgstbase-1.0 -lgstreamer-1.0 -lgobject-2.0 -lglib-2.0 -lgstvideo-1.0 -lgio-2.0 -lvncclient \
      -Wl,--end-group
```

# Handling connection failures from Python

Applications should listen for `Gst.MessageType.ERROR` on the pipeline bus.
Connection failures from this element include normal GStreamer error domain/code
information and an `rfbsrc-error` details structure with these fields:

- `reason`: machine-readable reason, for example `connection-failed`,
  `authentication-failed`, `connection-lost`, `timeout`,
  `first-frame-timeout`, `security-negotiation-failed`, `protocol-error`,
  `host-empty`, `invalid-framebuffer-size`, `capture-area-invalid`,
  `setup-failed`, `caps-allocation-failed`, `caps-negotiation-failed`,
  `framebuffer-update-request-failed`, `server-message-wait-failed`,
  `server-message-failed`, `no-framebuffer`, `allocation-failed`,
  `output-buffer-allocation-failed`, or `output-buffer-map-failed`
- `stage`: lifecycle stage such as `connect`, `initialise`, `set-format`,
  `validate-geometry`, `negotiate-caps`, `request-frame`, `wait-frame`,
  `wait-message`, `handle-message`, `allocate-buffer`, or `copy-frame`
- `host` / `port`: the target VNC endpoint
- `libvnc-error`: LibVNCClient error text when available

Example using PyGObject:

``` python
from gi.repository import Gst

Gst.init(None)

pipeline = Gst.parse_launch(
    "rfbsrc host=127.0.0.1 port=5900 password=secret ! fakesink"
)
bus = pipeline.get_bus()

pipeline.set_state(Gst.State.PLAYING)

while True:
    msg = bus.timed_pop_filtered(
        Gst.CLOCK_TIME_NONE,
        Gst.MessageType.ERROR | Gst.MessageType.EOS,
    )

    if msg.type == Gst.MessageType.ERROR:
        err, debug = msg.parse_error()
        details = msg.parse_error_details()

        reason = details.get_string("reason") if details else None
        stage = details.get_string("stage") if details else None
        libvnc_error = (
            details.get_string("libvnc-error")
            if details and details.has_field("libvnc-error")
            else None
        )

        print("rfbsrc failed:", reason, stage, err.message, libvnc_error)
        break

    if msg.type == Gst.MessageType.EOS:
        break

pipeline.set_state(Gst.State.NULL)
```

# ATTENTION
As this plugin is built against other libs (e.g. libvncclient) be carefull when building this application again on different operating systems since those might ship different versions
of those libs in their package repos.
