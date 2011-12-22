Cygwin spawn helper (Cygspawn)
==============================

Cygwin uses posix paths and environments which makes most of
the standard windows programs to fail because of path missmatch.
The traditional way of handling that is using Cygwin's cygpath
utility which translates cygwin (posix) paths to their windows
equivalents from shell.

For example a standard usage would be:

    program.exe "--f1=`cygpath -w /tmp/f1`" "`cygpath -w /tmp/f1`" ...
  
This can become very complex and it requires that the shell
script is aware it runs inside the cygwin environment.

Cygpath utility does that automatically by replacing each posix
argument that contains path element with its windows equvalent.
It also replaces paths in the environment variable values making
sure the miltiple path elemnts are correctly separated using
windows path separator ';'.
