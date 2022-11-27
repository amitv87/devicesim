set -e
# set -x

CUR_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"

mkdir -p $DEPS_BUILD_PATH;

put(){
  echo -e "\033[92m$@\033[0m"
}

enter(){
  LIB=$1
  LIB_SRC_DIR=$CUR_DIR/$LIB;
  LIB_BUILD_DIR=$DEPS_BUILD_PATH/$LIB;
  mkdir -p $LIB_BUILD_DIR;
  cd $LIB_BUILD_DIR;
  echo $LIB_BUILD_DIR
}

build(){
  make -j$NUM_CPU $@
}

cbuild(){
  cmake $LIB_SRC_DIR -DCMAKE_BUILD_TYPE=Release $@
}

build_static_lib(){
  LIB_NAME=$2
  FILES=(${3})
  CFLAGS=$4
  put "Building $LIB_NAME"
  enter $1

  cd $LIB_SRC_DIR
  for file in "${FILES[@]}"
  do
    put "Building C object $file"
    mkdir -p $LIB_BUILD_DIR/"$(dirname $file)"
    gcc $CFLAGS $file -c -o $LIB_BUILD_DIR/$file.o
  done
  cd $LIB_BUILD_DIR
  find . -type f -name '*.o' | xargs ar rcs $LIB_NAME
  put "Built $LIB_NAME"
  ls -la $LIB_NAME
}

enter libusb
if [[ ! -f "libtool" ]] ; then
  put "Configuring $LIB"
  bash $LIB_SRC_DIR/bootstrap.sh > /dev/null
  $LIB_SRC_DIR/configure CFLAGS="-O3 -fPIC -DNDEBUG" > /dev/null
fi
build
