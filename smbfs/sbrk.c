/*
 * sbrk()
 */

#include <x68k/dos.h>
#include <x68k/iocs.h>
#include <unistd.h>
#include <errno.h>

char *_HSTA;
char *_HEND;

//#define SBRK_DEBUG

#ifdef SBRK_DEBUG
void puthex(int v)
{
  for (int i = 0; i < 8; i++) {
    int d = (v >> 28) & 0xf;
    char c = (d < 10) ? ('0' + d) : ('A' + (d - 10));
    _iocs_b_putc(c);
    v <<= 4;
  }
}
#endif

void *sbrk(ptrdiff_t incr)
{
  static char *heap_end;
  char *new_heap_end;
  char *prev_heap_end;

  if (heap_end == 0)
    heap_end = _HSTA;
 
  prev_heap_end = heap_end;
  new_heap_end = heap_end + incr;

  if (new_heap_end > _HEND) {
    errno = ENOMEM;
    return (void *)-1;
  }

#ifdef SBRK_DEBUG
  _iocs_b_print("sbrk: size=");
  puthex((int)incr);
  _iocs_b_print(" prev=");
  puthex((int)prev_heap_end);
  _iocs_b_print(" new=");
  puthex((int)new_heap_end);
  _iocs_b_print("\r\n");
#endif

  heap_end = new_heap_end;

  return (void *)prev_heap_end;
}
