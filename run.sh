set -e
# set -x

PROJECT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"

SRC_ROOT_DIR=$PROJECT_DIR

cd $SRC_ROOT_DIR

PROJECT_PATH="$SRC_ROOT_DIR"

BUILD_PATH="$SRC_ROOT_DIR/.build"
[[ ! -z "${getDevBuildPath}" ]] && eval "$getDevBuildPath" && BUILD_PATH=$(getDevBuildPath);

[[ -z "$NUM_CPU" ]] && NUM_CPU=$(getconf _NPROCESSORS_ONLN)

SILENT_MAKE="-s"
# VERBOSE_ARGS="-DCMAKE_VERBOSE_MAKEFILE=TRUE"

run(){
  cd $BUILD_PATH
  ./$@
  # | TZ=UTC ts '[%F %.T]'
  cd ~-
}

debug(){
  cd $BUILD_PATH
  lldb $@
  # | TZ=UTC ts '[%F %.T]'
  cd ~-
}

printSize(){
  echo BUILD_PATH: $BUILD_PATH
  cd $BUILD_PATH

  exec_files_array=(${@})
  if [ -z "$exec_files_array" ]; then
    executables=$(find . -maxdepth 1 -executable -type f -printf '%f ' || find . -maxdepth 1 -perm +0111 -type f)
    if [ ! -z "$executables" ]; then
      echo $executables | xargs md5
      echo $executables | xargs size
      echo $executables | xargs ls -la
    fi
    return
  fi

  for exec in "${exec_files_array[@]}"
  do
    md5 $exec
    size $exec
    ls -la $exec
  done
  cd ~-
}

build(){
  mkdir -p $BUILD_PATH
  cd $BUILD_PATH
  [[ ! -f "Makefile" ]] && cmake $VERBOSE_ARGS $PROJECT_PATH
  make $SILENT_MAKE -j $NUM_CPU $@
  cd ~-
}

compile(){
  build $@;
  printSize $@;
}

ACTION="$1"

shift;

if [[ $ACTION == "c" ]]; then
  build clean;
elif [[ $ACTION == "b" ]]; then
  compile $@;
elif [[ $ACTION == "br" ]]; then
  app=$1
  shift;
  compile $app;
  run $app $@;
elif [[ $ACTION == "bd" ]]; then
  app=$1
  shift;
  compile $app;
  debug $app $@;
else
  run $ACTION $@
fi
