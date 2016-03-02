#!/bin/sh

pushd "$(dirname $0)" >/dev/null
../flatc -b monster_test.fbs unicode_test.json
skewc ../skew/*.sk *.sk --output-file=SkewTest.js
node SkewTest.js
skewc ../skew/*.sk *.sk --output-file=SkewTest.min.js --inline-functions --fold-constants --globalize-functions --js-mangle
node SkewTest.min.js
