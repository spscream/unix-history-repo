/@(#)depend.sed	7.1/s//@(#)depend.sed	7.1/
s/:.*include[ 	]*/:/
/\.c/s/\.c/.o/
/"\([^"]*\).*/s//\1/gp
/<access.h>/s;<\(.*\)>.*;\$H/\1;p
/<aux.h>/s;<\(.*\)>.*;\$H/\1;p
/<catalog.h>/s;<\(.*\)>.*;\$H/\1;p
/<func.h>/s;<\(.*\)>.*;\$H/\1;p
/<ingres.h>/s;<\(.*\)>.*;\$H/\1;p
/<lock.h>/s;<\(.*\)>.*;\$H/\1;p
/<pmon.h>/s;<\(.*\)>.*;\$H/\1;p
/<opsys.h>/s;<\(.*\)>.*;\$H/\1;p
/<pv.h>/s;<\(.*\)>.*;\$H/\1;p
/<range.h>/s;<\(.*\)>.*;\$H/\1;p
/<resp.h>/s;<\(.*\)>.*;\$H/\1;p
/<symbol.h>/s;<\(.*\)>.*;\$H/\1;p
/<trace.h>/s;<\(.*\)>.*;\$H/\1;p
/<tree.h>/s;<\(.*\)>.*;\$H/\1;p
/<useful.h>/s;<\(.*\)>.*;\$H/\1;p
/<version.h>/s;<\(.*\)>.*;\$H/\1;p
