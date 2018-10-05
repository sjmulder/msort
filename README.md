msort
=====

This program is a top down merge sort implementation for sorting the
lines of a file:

    msort <unsorted.txt >sorted.txt


Implementation
--------------

Refer to external documentation for more information about merge sort in
general, but here are specifics regarding this implementation:

 - The data representation is the original file plus a terminating \n, hence
   it can be considered a packed list of \n-terminated strings. This avoids
   indirection (of a pointer list) and preprocessing.

 - Merge sort practically requires using a second buffer for the sorting
   steps. The roles of main and scratch buffer are swapped at each step of
   recursion in such a way that the main buffer ends up sorted in the end.

 - Two methods of parallelism are implemented: pthreads and forking. (Both
   are configured in the main function.) Since the main and scratch buffers
   use shared memory, no communication is required other than waiting for
   the thread or fork to complete.

More documentation can be found in the code.


Running
-------

Tested on OpenBSD and Debian. Required packages on Debian, apart from the
usual build tools: *wamerican*, *time*, and *entr*.

    make test

    # uses a larger file but does no verification
    make bench

    # watch targets with entr
    make watch-test
    make watch-bench


Legal
-----

Copyright (c) 2018, Sijmen J. Mulder <ik@sjmulder.nl>

msort is free software: you can redistribute it and/or modify it under
the terms of the GNU Affero General Public License as published by the Free
Software Foundation, either version 3 of the License, or (at your option)
any later version.

msort is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for
more details.

You should have received a copy of the GNU Affero General Public License
along with msort. If not, see <https://www.gnu.org/licenses/>.
