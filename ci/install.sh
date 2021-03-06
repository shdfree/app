#!/bin/bash

sudo add-apt-repository -y ppa:ubuntu-toolchain-r/test
sudo apt-get update
sudo apt-get --quiet --yes install \
  build-essential \
  git \
  autoconf \
  libtool \
  pkg-config \
  gperf \
  gettext \
  python-lxml \
  libgd2-xpm \
  ia32-libs \
  ia32-libs-multiarch \
  p7zip \
  checkinstall \
  libgd2-xpm \
  ia32-libs \
  ia32-libs-multiarch \
  libstdc++6-4.7-dev \
  rpl >/dev/null

echo "$ANDROID_HOME"

cd $ANDROID_HOME || exit 1

wget --timeout=120 https://dl.google.com/android/repository/android-ndk-r14b-linux-x86_64.zip -O ndk.zip >/dev/null
unzip ndk.zip >/dev/null
mv android-ndk-* ndk-bundle
export ANDROID_NDK="${ANDROID_HOME}ndk-bundle"
export ANDROID_NDK_HOME=$ANDROID_HOME/ndk-bundle
export PATH=${PATH}:${ANDROID_HOME}/tools:${ANDROID_HOME}/platform-tools:${ANDROID_NDK_HOME}

echo "sdk.dir=$ANDROID_HOME" > $TRAVIS_BUILD_DIR/local.properties
echo "ndk.dir=$ANDROID_NDK_HOME" > $TRAVIS_BUILD_DIR/local.properties

cd - || exit 1

export ADB_INSTALL_TIMEOUT=5

mkdir "$ANDROID_HOME/licenses" || true
echo -e "\n8933bad161af4178b1185d1a37fbf41ea5269c55" > "$ANDROID_HOME/licenses/android-sdk-license"
echo -e "\n84831b9409646a918e30573bab4c9c91346d8abd" > "$ANDROID_HOME/licenses/android-sdk-preview-license"

echo y | android update sdk -u -a -t tools
echo y | android update sdk -u -a -t platform-tools
echo y | android update sdk -u -a -t build-tools-$ANDROID_BUILD_TOOLS
echo y | android update sdk -u -a -t android-$ANDROID_API
echo y | android update sdk -u -a -t ndk-bundle
echo y | android update sdk -u -a -t cmake
echo y | android update sdk -u -a -t lldb
echo y | android update sdk -u -a -t extra-google-m2repository
echo y | android update sdk -u -a -t extra-android-m2repository

bash $TRAVIS_BUILD_DIR/distribution/get_native_dependencies.sh
