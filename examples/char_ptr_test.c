/* char_ptr_test.c -- regression test for DEBUGGING_NOTES.md #38 (byte-address
 * pointers, packed chars via LDB/STB). Prints ok=38 bad=0 when correct; the
 * pre-#38 compiler failed 15+ checks and then crashed. */
#include <stdio.h>
#include <string.h>
unsigned char gz[8];                 /* zero-init global */
char gs[8] = "abcdefg";              /* string-initialized global */
const char *gp = "hello";            /* pointer-to-literal global */
char *gptrs[2] = { gs, gs + 3 };     /* pointers into a global, with offset */
struct S { char a; char b; int x; char c; } gst = { 'P', 'Q', 1234, 'R' };
int ia[8] = {10,11,12,13,14,15,16,17};
int ok = 0, bad = 0;
static void check(const char *what, int got, int want) {
  if (got == want) ok++; else { bad++; printf("FAIL %s: got %d want %d\n", what, got, want); }
}
static int idx(const char *p, int i) { return p[i]; }
static int walk(const char *p) { int n = 0; while (*p) { n += *p; p++; } return n; }
static void fill(char *buf, int n, char c) { int i; for (i = 0; i < n; i++) buf[i] = (char)(c + i); buf[n] = 0; }
static int isum(int *p, int n) { int s = 0; while (n--) { s += *p; p++; } return s; }
static int *adv(int *p, int k) { return p + k; }
int main(void) {
  volatile int k = 3;
  char loc[8];
  struct S ls;
  char buf[16], buf2[16];
  signed char sc = -5;
  int i;
  gz[k] = 7; check("gz[k]", gz[k], 7); check("gz[k-1]", gz[k-1], 0); check("gz[k+1]", gz[k+1], 0);
  check("gs[k]", gs[k], 'd'); check("gs[0]", gs[0], 'a'); check("gs[6]", gs[6], 'g');
  for (i = 0; i < 4; i++) loc[i] = (char)('w' + i);
  check("loc[0]", loc[0], 'w'); check("loc[1]", loc[1], 'x'); check("loc[k]", loc[k], 'z');
  check("&loc[1]-&loc[0]", (int)(&loc[1] - &loc[0]), 1);
  check("idx(gs,k)", idx(gs, k), 'd'); check("idx(loc,1)", idx(loc, 1), 'x'); check("idx(gp,4)", idx(gp, 4), 'o');
  check("walk(gp)", walk(gp), 'h'+'e'+'l'+'l'+'o');
  check("*gptrs[1]", *gptrs[1], 'd'); check("gptrs[0][1]", gptrs[0][1], 'b');
  check("gst.a", gst.a, 'P'); check("gst.b", gst.b, 'Q'); check("gst.x", gst.x, 1234); check("gst.c", gst.c, 'R');
  ls.a = 'a'; ls.b = 'b'; ls.x = -7; ls.c = 'c';
  check("ls.a", ls.a, 'a'); check("ls.b", ls.b, 'b'); check("ls.x", ls.x, -7); check("ls.c", ls.c, 'c');
  check("sizeof(struct S)", sizeof(struct S), 6); check("sizeof(gs)", sizeof(gs), 8);
  check("sc", sc, -5);
  fill(buf, 5, 'A'); check("strlen(buf)", (int)strlen(buf), 5); check("buf[4]", buf[4], 'E');
  strcpy(buf2, buf); check("strcpy", buf2[2], 'C'); check("strcmp", strcmp(buf, buf2), 0);
  memcpy(buf2, gs, 8); check("memcpy", buf2[k], 'd');
  check("strlen(gp)", (int)strlen(gp), 5); check("strlen(gs)", (int)strlen(gs), 7);
  check("isum", isum(ia, 8), 108); check("adv", *adv(ia, k), 13);
  { int *p = ia, *q = &ia[5]; check("q-p", (int)(q - p), 5); p += k; check("p+=k", *p, 13); }
  printf("%s %d\n", "printf-s", 42);
  printf("ok=%d bad=%d\n", ok, bad);
  return 0;
}
