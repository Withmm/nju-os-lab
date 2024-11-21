#include "co.h"
#include <stdlib.h>
#include <setjmp.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#define alignment16(a) ((a) & (~(16 - 1)))
#define STACK_SIZE 	(4096)
#define NAME_SIZE 	(32)
#define CO_SIZE   	(256)
static struct co *search_from_current();
static struct co *add_ctb(struct co *conode);
static inline void
stack_switch_call(void *sp, void *entry, uintptr_t arg1, uintptr_t arg2) {
    asm volatile (
#if __x86_64__
        "movq %0, %%rsp; movq %2, %%rdi; movq %3, %%rsi; jmp *%1"
          :
          : "b"((uintptr_t)sp),
            "d"(entry),
            "a"(arg1),
	    "c"(arg2)
          : "memory"
#else
        "movl %0, %%esp; movl %2, 4(%0); movl %3, 8(%0); jmp *%1"
          :
          : "b"((uintptr_t)sp),
            "d"(entry),
            "a"(arg1),
	    "c"(arg2)
          : "memory"
#endif
    );
}

struct co *co_table[CO_SIZE];
enum co_status {
	CO_NEW = 1,
	CO_RUNNING,
	CO_WAITING,
	CO_DEAD,
};

struct co {
	char *name;
	void (*func)(void *);
	void *arg;

	enum co_status status;

	struct co *waiter;
	jmp_buf context;
	uint8_t stack[STACK_SIZE];

	int idx;
};

struct co *current;
void __attribute__((force_align_arg_pointer)) coroutine_wrapper(void (*func)(void *), void *arg)
{
	func(arg);
	current->status = CO_DEAD;
	if (current->waiter)
		current->waiter->status = CO_RUNNING;
	co_yield();
}

struct co *co_start(const char *name, void (*func)(void *), void *arg)
{
	struct co *conode = malloc(sizeof(struct co));
	if (conode == NULL)
		return NULL;
	conode->name = malloc(NAME_SIZE);
	if (conode->name == NULL)
		return NULL;
	memset(conode->name, 0, NAME_SIZE);
	if (strlen(name) >= NAME_SIZE)
		return NULL;
	strcpy(conode->name, name);
	conode->func = func;
	conode->arg = arg;
	conode->status = CO_NEW;
	// jmp_buf is 0
	memset(conode->stack, 0, sizeof(conode->stack));

	// add to global co table
	return add_ctb(conode);
}

void co_wait(struct co *co)
{
	while (co->status != CO_DEAD) {
		if (!current)
			co_yield();
		current->status = CO_WAITING;
		co->waiter = current;
		co_yield();
	}

	co_table[co->idx] = NULL;
	free(co->name);
	free(co);
}

void co_yield()
{
	if (!current) { //add main to coroutine
		struct co* comain = malloc(sizeof(struct co));
		comain->name = malloc(NAME_SIZE);
		memset(comain->name, 0, NAME_SIZE);
		strcpy(comain->name, "main_coroutine");

		comain->status = CO_RUNNING;

		add_ctb(comain);
		current = comain;
	}
	int val = setjmp(current->context);
	if (val == 0) {
		struct co *c = search_from_current();
		if (c == NULL) //all coroutines end
			return ;
		current = c;
		if (current->status == CO_NEW) { // new coroutine
			current->status = CO_RUNNING;
			//printf("stack_switch_call...\n");
			void *now = (void *)alignment16(((uintptr_t)(current->stack) + STACK_SIZE));
			stack_switch_call(now, coroutine_wrapper, (uintptr_t)current->func, (uintptr_t)current->arg);
		}
		longjmp(current->context, 1);
	}
}

static struct co *search_from_current()
{
	for (int i = (current->idx + 1) % CO_SIZE; i < CO_SIZE; i = (i + 1) % CO_SIZE) {
		struct co *tmp = co_table[i];
		if (tmp && (tmp->status == CO_NEW || tmp ->status == CO_RUNNING))
			return tmp;
	}
	return NULL;
}

static struct co *add_ctb(struct co *conode)
{
	int free_idx = 0;
	while(co_table[free_idx] != NULL && free_idx < CO_SIZE) {
		free_idx++;
	}
	if (free_idx == CO_SIZE) {
	// co pool is full
		free(conode->name);
		free(conode);
		return NULL;
	}
	conode->idx =free_idx;
	co_table[free_idx] = conode;
	return conode;
}
