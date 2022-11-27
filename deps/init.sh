set -e
# set -x

CUR_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"
cd $CUR_DIR

if [[ -f "../.gitmodules" ]] ; then
  echo "syncing gitmodules"
  git submodule deinit -f .
  if [ "$(uname)" == "Darwin" ] ; then
    GIT_ARGS="--jobs 10"
  fi
  git submodule update --init --recursive $GIT_ARGS
fi

sync_lib_zip(){
  lib_name=$1
  lib_version=$2
  lib_url=$3
  if [[ ! -d "$lib_name" ]] ; then
    echo "syncing $lib_name@$lib_version"
    curl -L $lib_url$lib_version.zip | jar xv && mv $lib_name-$lib_version $lib_name
  fi
}

sync_lib_zip libusb 1.0.26 https://github.com/libusb/libusb/archive/refs/tags/v
