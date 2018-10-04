msort
=====

This program is a top down merge sort implementation for sorting the
lines of a file lexographically:

    msort <unsorted.txt >sorted.txt


Implementation
--------------

Refer to external documentation for more information about merge sort in
general, but here are specifics regarding this implementation:

 - The data representation is the original file (plus a terminating \0).
   There is no line pointer table or such. This complicates the code but
   removes a layer of indirection. (Embedded \0 characters break the
   program.)

 - Two methods of parallelism are implemented: pthreads and forking. (Both
   are configured in the main function.) Since work is independent no
   communication with other threads is required other than joining. Forks
   pass results back through a named pipe.

 - Merge sort practically requires using a second buffer for the sorting
   steps. The roles of main and scratch buffer are swapped at each step of
   recursion in such a way that the main buffer ends up sorted in the end.

More documentation can be found in the code.


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
