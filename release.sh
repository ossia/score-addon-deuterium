#!/bin/bash
rm -rf release
mkdir -p release

cp -rf Deuterium 3rdparty *.cpp *.txt release/
# The submodule's .git file breaks unzipped-source builds
rm -rf release/3rdparty/libgig-modified/.git

mv release score-addon-deuterium
7z a score-addon-deuterium.zip score-addon-deuterium
