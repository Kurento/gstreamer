noinst_PROGRAMS = schedule-tasks stream-status
if HAVE_PTHREAD
noinst_PROGRAMS += rtpool-test
endif

stream_status_SOURCES = stream-status.c
stream_status_LDADD = $(GST_OBJ_LIBS)
stream_status_CFLAGS = $(GST_OBJ_CFLAGS)

rtpool_test_SOURCES = rtpool-test.c testrtpool.h testrtpool.c
rtpool_test_LDADD = $(GST_OBJ_LIBS) $(PTHREAD_LIBS)
rtpool_test_CFLAGS = $(GST_OBJ_CFLAGS) $(PTHREAD_CFLAGS)

schedule_tasks_SOURCES = schedule-tasks.c
schedule_tasks_LDADD = $(GST_OBJ_LIBS)
schedule_tasks_CFLAGS = $(GST_OBJ_CFLAGS)

EXTRA_DIST = rtpool-test.c testrtpool.h testrtpool.c
