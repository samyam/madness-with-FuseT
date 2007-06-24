#!/bin/csh

set files = (`find . \( -name "*.h" -o -name "*.cc" -o -name "*.c" -o -name "*.hpp" -o -name "*.cpp" \)`)
set files = (`grep -L 'This program is free software' $files | grep -v mainpage | grep -v lookup3 | grep -v LIBS`)

foreach file ($files)
  echo Processing $file
  cat config/HEADER $file > tmptmptmp
  mv tmptmptmp $file
end


