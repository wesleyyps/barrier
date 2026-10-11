FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# Instalando dependências básicas do Barrier e de compilação
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    pkg-config \
    libcurl4-openssl-dev \
    libssl-dev \
    libavahi-compat-libdnssd-dev \
    libx11-dev \
    libxtst-dev \
    libxrandr-dev \
    libxi-dev \
    qtbase5-dev \
    qttools5-dev \
    qttools5-dev-tools \
    # Dependências para Wayland/DBus/libei
    libdbus-1-dev \
    libwayland-dev \
    libxkbcommon-dev \
    libei-dev \
    # Test dependencies:
    xvfb xdotool ydotool \
    mutter xdg-desktop-portal xdg-desktop-portal-gnome dbus-x11 \
    sudo \
    && rm -rf /var/lib/apt/lists/*

# Configurando um usuário não-root (desenvolvedor)
ARG USER_ID=1000
ARG GROUP_ID=1000
RUN (userdel -r ubuntu || true) && \
    groupadd -g ${GROUP_ID} developer && \
    useradd -u ${USER_ID} -g developer -m -s /bin/bash developer && \
    echo "developer ALL=(ALL) NOPASSWD: ALL" > /etc/sudoers.d/developer

USER developer
WORKDIR /workspace

CMD ["/bin/bash"]
