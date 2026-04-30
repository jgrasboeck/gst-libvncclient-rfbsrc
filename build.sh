gcc -c -fPIC /tmp/gst-libvncclient-rfbsrc/gstrfbsrc.c \
      -o /tmp/gst-libvncclient-rfbsrc/gstrfbsrc.o \
      -DVERSION=\"1.0.0\" -DPACKAGE=\"gst-libvncclient-rfbsrc\" \
      -DGST_LICENSE=\"LGPL\" \
      -DGST_PACKAGE_NAME=\"gst-libvncclient-rfbsrc\" \
      -DGST_PACKAGE_ORIGIN=\"https://github.com/jgrasboeck/gst-libvncclient-rfbsrc\" \
      $(pkg-config --cflags gstreamer-1.0 gstreamer-base-1.0 gstreamer-video-1.0) &&
gcc -c -fPIC /tmp/gst-libvncclient-rfbsrc/rfbsrc-keymap.c \
      -o /tmp/gst-libvncclient-rfbsrc/rfbsrc-keymap.o \
      -DVERSION=\"1.0.0\" -DPACKAGE=\"gst-libvncclient-rfbsrc\" \
      -DGST_LICENSE=\"LGPL\" \
      -DGST_PACKAGE_NAME=\"gst-libvncclient-rfbsrc\" \
      -DGST_PACKAGE_ORIGIN=\"https://github.com/jgrasboeck/gst-libvncclient-rfbsrc\" \
      $(pkg-config --cflags gstreamer-1.0 gstreamer-base-1.0 gstreamer-video-1.0) &&
gcc -o /usr/lib/aarch64-linux-gnu/gstreamer-1.0/libgstrfbsrc.so \
      -shared /tmp/gst-libvncclient-rfbsrc/gstrfbsrc.o /tmp/gst-libvncclient-rfbsrc/rfbsrc-keymap.o \
      -Wl,--as-needed -Wl,--no-undefined -fPIC \
      -Wl,--start-group -lgstbase-1.0 -lgstreamer-1.0 -lgobject-2.0 -lglib-2.0 -lgstvideo-1.0 -lgio-2.0 -lvncclient \
      -Wl,--end-group