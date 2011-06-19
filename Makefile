VERS = 2.1.10

CFLAGS  =  -Wall -I/usr/include \
           -I/home/angelo/progetti/freeradius-server-$(VERS)/src

LIBS    =  -lc -lxmlrpc -lxmlrpc_client

ALL:    rlm_xmlrpc.o rlm_xmlrpc-$(VERS).so

rlm_xmlrpc.o:     rlm_xmlrpc.c
	cc -g -fPIC -DPIC -c $(CFLAGS) rlm_xmlrpc.c

rlm_xmlrpc-$(VERS).so:    rlm_xmlrpc.o
	cc -g -shared -Wl,-soname,rlm_xmlrpc-$(VERS).so \
		-o rlm_xmlrpc-$(VERS).so rlm_xmlrpc.o $(LIBS)

install:        ALL
	install rlm_xmlrpc-$(VERS).so /usr/lib/freeradius
	ln -fs rlm_xmlrpc-$(VERS).so /usr/lib/freeradius/rlm_xmlrpc.so
	install rlm_xmlrpc.5 /usr/share/man/man5/rlm_xmlrpc.5 

clean:
	rm rlm_xmlrpc*.o rlm_xmlrpc*.so
