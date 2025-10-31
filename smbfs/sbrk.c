/*
 * sbrk()
 */

#include <x68k/dos.h>
#include <x68k/iocs.h>
#include <unistd.h>
#include <errno.h>

void puthex(int v)
{
  for (int i = 0; i < 8; i++) {
    int d = (v >> 28) & 0xf;
    char c = (d < 10) ? ('0' + d) : ('A' + (d - 10));
    _iocs_b_putc(c);
    v <<= 4;
  }
}

void *sbrk(ptrdiff_t incr)
{
  extern char *_HSTA, *_HEND;     /* Set by linker.  */
  static char *heap_end;
  char *new_heap_end;
  char *prev_heap_end;

  if (heap_end == 0)
    heap_end = _HSTA;
 
  prev_heap_end = heap_end;
  new_heap_end = heap_end + incr;

  if (new_heap_end > _HEND) {
    _iocs_b_print("sbrk: need to extend heap\r\n");
    while (1)
      ;
  }

  _iocs_b_print("sbrk: ");
  puthex((int)incr);
  _iocs_b_print(" ");
  puthex((int)prev_heap_end);
  _iocs_b_print(" ");
  puthex((int)new_heap_end);
  _iocs_b_print("\r\n");
  heap_end = new_heap_end;

  return (void *)prev_heap_end;
}
