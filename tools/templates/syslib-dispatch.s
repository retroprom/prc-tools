/* DO NOT EDIT!
   This file was automatically generated by @progname@
   from @deffile@  */

	.file	"@fname@"

/* This file contains the dispatch table for this system shared library.
   The entry point of the library's code resource must be a function that
   initialises the dispatchTblP field of its entryP argument to the jmptable
   provided in this file.  (Typically the function will also initialise the
   globalsP member.)  Something like this is suitable:

	Err
	start (UInt16 refnum, SysLibTblEntryPtr entryP) {
	  extern void *jmptable ();
	  entryP->dispatchTblP = (void *) jmptable;
	  entryP->globalsP = NULL;
	  return 0;
	  }

   You need to use the -nostartfiles option when linking, or linking will
   fail due to conflicts with the normal application startup code's entry
   point.  */

.text
	.even
	.globl jmptable
jmptable:
	dc.w	.Lname-jmptable
	@-function-offsets-@

.Lname:
	.asciz	"@libname@"
