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

# ATTENTION
As this plugin is built against other libs (e.g. libvncclient) be carefull when building this application again on different operating systems since those might ship different versions
of those libs in their package repos.