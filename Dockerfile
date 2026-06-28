FROM debian:bookworm-slim

RUN dpkg --add-architecture armhf && \
    apt-get update && \
    apt-get install -y \
        crossbuild-essential-armhf \
        libasound2-dev:armhf \
        libegl1-mesa-dev:armhf \
        libgles2-mesa-dev:armhf \
        pkg-config:armhf \
        git make wget \
    && apt-get clean && \
    rm -rf /var/lib/apt/lists/*

# Custom eglplatform.h with void* types (compatible with Mali fbdev)
RUN mkdir -p /usr/arm-linux-gnueabihf/include/EGL && \
    echo '#ifndef __eglplatform_h_' > /usr/arm-linux-gnueabihf/include/EGL/eglplatform.h && \
    echo '#define __eglplatform_h_' >> /usr/arm-linux-gnueabihf/include/EGL/eglplatform.h && \
    echo '#include <KHR/khrplatform.h>' >> /usr/arm-linux-gnueabihf/include/EGL/eglplatform.h && \
    echo '' >> /usr/arm-linux-gnueabihf/include/EGL/eglplatform.h && \
    echo '#ifndef EGLAPI' >> /usr/arm-linux-gnueabihf/include/EGL/eglplatform.h && \
    echo '#define EGLAPI KHRONOS_APICALL' >> /usr/arm-linux-gnueabihf/include/EGL/eglplatform.h && \
    echo '#endif' >> /usr/arm-linux-gnueabihf/include/EGL/eglplatform.h && \
    echo '#ifndef EGLAPIENTRY' >> /usr/arm-linux-gnueabihf/include/EGL/eglplatform.h && \
    echo '#define EGLAPIENTRY KHRONOS_APIENTRY' >> /usr/arm-linux-gnueabihf/include/EGL/eglplatform.h && \
    echo '#define EGLAPIENTRYP EGLAPIENTRY*' >> /usr/arm-linux-gnueabihf/include/EGL/eglplatform.h && \
    echo '#endif' >> /usr/arm-linux-gnueabihf/include/EGL/eglplatform.h && \
    echo '' >> /usr/arm-linux-gnueabihf/include/EGL/eglplatform.h && \
    echo 'typedef void* EGLNativeDisplayType;' >> /usr/arm-linux-gnueabihf/include/EGL/eglplatform.h && \
    echo 'typedef void* EGLNativePixmapType;' >> /usr/arm-linux-gnueabihf/include/EGL/eglplatform.h && \
    echo 'typedef void* EGLNativeWindowType;' >> /usr/arm-linux-gnueabihf/include/EGL/eglplatform.h && \
    echo 'typedef void* NativeDisplayType;' >> /usr/arm-linux-gnueabihf/include/EGL/eglplatform.h && \
    echo 'typedef void* NativePixmapType;' >> /usr/arm-linux-gnueabihf/include/EGL/eglplatform.h && \
    echo 'typedef void* NativeWindowType;' >> /usr/arm-linux-gnueabihf/include/EGL/eglplatform.h && \
    echo 'typedef khronos_int32_t EGLint;' >> /usr/arm-linux-gnueabihf/include/EGL/eglplatform.h && \
    echo '#define EGL_CAST(type, value) ((type)(value))' >> /usr/arm-linux-gnueabihf/include/EGL/eglplatform.h && \
    echo '#endif' >> /usr/arm-linux-gnueabihf/include/EGL/eglplatform.h

# Mali fbdev native window header
RUN echo '/* Mali fbdev native window */' > /usr/arm-linux-gnueabihf/include/EGL/fbdev_window.h && \
    echo 'struct fbdev_window {' >> /usr/arm-linux-gnueabihf/include/EGL/fbdev_window.h && \
    echo '    unsigned short width;' >> /usr/arm-linux-gnueabihf/include/EGL/fbdev_window.h && \
    echo '    unsigned short height;' >> /usr/arm-linux-gnueabihf/include/EGL/fbdev_window.h && \
    echo '};' >> /usr/arm-linux-gnueabihf/include/EGL/fbdev_window.h

WORKDIR /work
