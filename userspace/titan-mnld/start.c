/*
 * The executable entry point for bionic, in place of the NDK's
 * crtbegin_dynamic.o (as bionic's own crtbegin.c does it): hand the
 * initial stack to __libc_init, which runs main() and exits. The dynamic
 * linker has already run the init arrays; there are no fini arrays here.
 */
struct structors_array {
	void (**preinit_array)(void);
	void (**init_array)(void);
	void (**fini_array)(void);
};

extern void __libc_init(void *raw_args, void (*onexit)(void),
			int (*slingshot)(int, char **, char **),
			const struct structors_array *structors) __attribute__((noreturn));
extern int main(void);

__attribute__((used, noreturn)) static void start_main(void *raw_args)
{
	static const struct structors_array none;

	__libc_init(raw_args, 0, (int (*)(int, char **, char **))main, &none);
}

__asm__(".text\n.align 4\n.type _start,%function\n.globl _start\n"
	"_start:\n\tmov r0, sp\n\tb start_main\n.size _start, .-_start\n");
