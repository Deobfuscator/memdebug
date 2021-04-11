.PHONY : clean

CFLAGS = -rdynamic -fPIC -std=c99 -Wall 
LIBS = -ldl -ldw -liberty

main : leak mymalloc.so

mymalloc.so : mymalloc.o
	$(LINK.cc) -shared $^ $(LIBS) -o $@

leak : leak.o
	$(LINK.cpp) $^ -o $@


clean : mymalloc.so mymalloc.o leak.o
	rm $^
