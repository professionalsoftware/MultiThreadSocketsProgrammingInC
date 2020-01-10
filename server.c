#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include "./comm.h"
#include "./db.h"
#ifdef __APPLE__
#include "pthread_OSX.h"
#endif

/*
 * Use the variables in this struct to synchronize your main thread with client
 * threads. Note that all client threads must have terminated before you clean
 * up the database.
 */
typedef struct server_control {
    pthread_mutex_t server_mutex;
    pthread_cond_t server_cond;
    int num_client_threads;
} server_control_t;

/*
 * Controls when the clients in the client thread list should be stopped and
 * let go.
 */
typedef struct client_control {
    pthread_mutex_t go_mutex;
    pthread_cond_t go;
    int stopped;
} client_control_t;

/*
 * The encapsulation of a client thread, i.e., the thread that handles
 * commands from clients.
 */
typedef struct client {
    pthread_t thread;
    FILE *cxstr;  // File stream for input and output

    // For client list
    struct client *prev;
    struct client *next;
} client_t;

/*
 * The encapsulation of a thread that handles signals sent to the server.
 * When SIGINT is sent to the server all client threads should be destroyed.
 */
typedef struct sig_handler {
    sigset_t set;
    pthread_t thread;
} sig_handler_t;

client_t *thread_list_head;
pthread_mutex_t thread_list_mutex = PTHREAD_MUTEX_INITIALIZER;

void *run_client(void *arg);
void *monitor_signal(void *arg);
void thread_cleanup(void *arg);

server_control_t  sct = {
	PTHREAD_MUTEX_INITIALIZER,
	PTHREAD_COND_INITIALIZER,
	0
};

client_control_t cct = {
	PTHREAD_MUTEX_INITIALIZER,
	PTHREAD_COND_INITIALIZER,
	0
};

// Called by client threads to wait until progress is permitted
void client_control_wait() {
    // TODO: Block the calling thread until the main thread calls
    // client_control_release(). See the client_control_t struct.
	pthread_mutex_lock (&(cct.go_mutex));
	while(cct.stopped != 0) {
		pthread_cond_wait (&(cct.go),&(cct.go_mutex));
	}
	pthread_mutex_unlock (&(cct.go_mutex));
}

// Called by main thread to stop client threads
void client_control_stop() {
    // TODO: Ensure that the next time client threads call client_control_wait()
    // at the top of the event loop in run_client, they will block.
	pthread_mutex_lock (&(cct.go_mutex));
	cct.stopped = 1;
	pthread_cond_signal (&(cct.go));
	pthread_mutex_unlock (&(cct.go_mutex));
}

// Called by main thread to resume client threads
void client_control_release() {
    // TODO: Allow clients that are blocked within client_control_wait()
    // to continue. See the client_control_t struct.
	pthread_mutex_lock (&(cct.go_mutex));
	cct.stopped = 0;
	pthread_cond_signal (&(cct.go));
	pthread_mutex_unlock (&(cct.go_mutex));
}

// Called by listener (in comm.c) to create a new client thread
void client_constructor(FILE *cxstr) {
    // You should create a new client_t struct here and initialize ALL
    // of its fields. Remember that these initializations should be
    // error-checked.
    //
    // TODO:
    // Step 1: Allocate memory for a new client and set its connection stream
    // to the input argument.
    client_t *new_client = (client_t *)malloc(sizeof(client_t));
    if (new_client == 0) {
      return;
    }

	new_client->cxstr = cxstr;
    // Step 2: Create the new client thread running the run_client routine.
    pthread_t thread;

    int err1 = pthread_create(&thread, 0, run_client, new_client);
    if (err1 != 0){
        handle_error_en(err1, "pthread_create");
    }

    int err2 = pthread_detach(thread);
    if (err2 != 0) {
		handle_error_en(err2, "pthread_detach");
	}

}

void client_destructor(client_t *client) {
    // TODO: Free all resources associated with a client.
    // Whatever was malloc'd in client_constructor should
    // be freed here!
    free(client);

	// decrease client threads.
	pthread_mutex_lock (&(sct.server_mutex));
	sct.num_client_threads--;
  
	if(sct.num_client_threads < 1) {
		// deleted all threads, so clean database
		pthread_cond_signal (&(sct.server_cond));
	}
	pthread_mutex_unlock (&(sct.server_mutex));
}

// Code executed by a client thread
void *run_client(void *arg) {
    // TODO:
    // Step 1: Make sure that the server is still accepting clients.
    client_t* new_client = (client_t*)arg;
    new_client->thread = pthread_self();
    
	pthread_mutex_lock (&thread_list_mutex);
    // Step 2: Add client to the client list and push thread_cleanup to remove
    //       it if the thread is canceled.
	new_client->thread = pthread_self();
    	if(thread_list_head == NULL) {
		thread_list_head = new_client;
		new_client->prev = NULL;
		new_client->next = NULL;
	}
	else {
		new_client->next = thread_list_head;
		thread_list_head->prev = new_client;
		new_client->prev = NULL;
		thread_list_head = new_client;
	}
    pthread_cleanup_push(thread_cleanup, new_client);

	pthread_mutex_unlock (&thread_list_mutex);
	// Increase the number of client threads.
	pthread_mutex_lock (&(sct.server_mutex));
	sct.num_client_threads++;
	pthread_mutex_unlock (&(sct.server_mutex));
    // Step 3: Loop comm_serve (in comm.c) to receive commands and output
    //       responses. Note that the client may terminate the connection at
    //       any moment, in which case reading/writing to the connection stream
    //       on the server side will send this process a SIGPIPE. You must
    //       ensure that the server doesn't crash when this happens!
	signal(SIGPIPE, SIG_IGN);
  
    char response[1024];
	char command[1024];
  
	while(1) {
		client_control_wait();
		if(comm_serve(new_client->cxstr, response, command) == 0) {
			// got a command.
			interpret_command(command, response, 1024);
		}
		else {
			break;
		}
	}
	return NULL;

    // Step 4: When the client is done sending commands, exit the thread
    //       cleanly.
	client_destructor(new_client);
    // Keep the signal handler thread in mind when writing this function!
	pthread_cleanup_pop(1);
    return NULL;
}

void delete_all() {
    // TODO: Cancel every thread in the client thread list with the
    // pthread_cancel function.

	pthread_mutex_lock (&thread_list_mutex);
  
	client_t* client = thread_list_head;
  
	while(client != NULL) {
		pthread_cancel(client->thread);
		client = client->next;
	}
  
	pthread_mutex_unlock (&thread_list_mutex);
}

// Cleanup routine for client threads, called on cancels and exit.
void thread_cleanup(void *arg) {
    // TODO: Remove the client object from thread list and call
    // client_destructor. This function must be thread safe! The client must
    // be in the list before this routine is ever run.
	pthread_mutex_lock (&thread_list_mutex);
    
	client_t* client = (client_t*)arg;
    if(client == thread_list_head) {
		thread_list_head = client->next;
		thread_list_head->prev = NULL;
	}
	else {
		client->prev->next = client->next;
		client->next->prev = client->prev;
	}
    pthread_mutex_unlock (&thread_list_mutex);
    
	client_destructor(client);
}

// Code executed by the signal handler thread. For the purpose of this
// assignment, there are two reasonable ways to implement this.
// The one you choose will depend on logic in sig_handler_constructor.
// 'man 7 signal' and 'man sigwait' are both helpful for making this
// decision. One way or another, all of the server's client threads
// should terminate on SIGINT. The server (this includes the listener
// thread) should not, however, terminate on SIGINT!
void *monitor_signal(void *arg) {
    // TODO: Wait for a SIGINT to be sent to the server process and cancel
    // all client threads when one arrives.

    sig_handler_t *signal_handler = (sig_handler_t*)arg;
	signal_handler->thread = pthread_self();
    sigset_t set = signal_handler->set;
    int sig;
    sigwait(&set, &sig);

    // program terminates with SIGINT
    delete_all();
    return NULL;
}

sig_handler_t *sig_handler_constructor() {
    // TODO: Create a thread to handle SIGINT. The thread that this function
    // creates should be the ONLY thread that ever responds to SIGINT.
    sig_handler_t *sig_handler = (sig_handler_t *)malloc(sizeof(sig_handler_t));
    sigset_t set;
    sigemptyset( &set );
    sigaddset( &set, SIGINT);
    sigaddset( &set, SIGPIPE);
    sig_handler -> set = set;

    pthread_sigmask(SIGINT,&set, 0);

    pthread_t thread;
    int err1 = pthread_create(&thread, 0, monitor_signal, sig_handler);
    if (err1 != 0){
        handle_error_en(err1, "pthread_create");
    }

    int err2 = pthread_detach(thread);
    if (err2 != 0) {
		handle_error_en(err2, "pthread_detach");
	}

    return sig_handler;
}

void sig_handler_destructor(sig_handler_t *sighandler) {
    // TODO: Free any resources allocated in sig_handler_constructor.
    free(sighandler);
}

// The arguments to the server should be the port number.
int main(int argc, char *argv[]) {
    // TODO:
    // Step 1: Set up the signal handler.
    sig_handler_t *sighandler = sig_handler_constructor();

    // Step 2: Start a listener thread for clients (see start_listener in comm.c).
    pthread_t listener = start_listener(atoi(argv[1]),client_constructor);

    // Step 3: Loop for command line input and handle accordingly until EOF.
    while(1){
		char *buffer;
		size_t bufsize = 1024;

		buffer = (char *)malloc(bufsize * sizeof(char));
		if( buffer == NULL)
		{
			perror("Unable to allocate buffer");
			exit(1);
		}
		
        size_t input = getline(&buffer, &bufsize, stdin);

        if (input == 0) {
                return 0;
        }

        else if (input == -1) {
            perror("read");
            return 1;
        }

         else if (input > 0) {
            // char *cmd = (char*)buffer[0];
            char *cmd = (char*)buffer;
            if(strncmp(cmd,"s", 1)==0){
                client_control_stop();
            }
            else if(strncmp(cmd,"g",1)==0){
                client_control_release();
            }
            else if(strncmp(cmd,"p",1)==0){
                
                if((strchr(cmd, 'p') + 1)!=NULL){
                    cmd= strchr(cmd, 'p') + 1;
                    C(cmd);
                }
                else{
                    db_print(NULL);
                }
                
                
            }
        }
    }

    // Step 4: Destroy the signal handler, delete all clients, cleanup the
    //       database, cancel the listener thread, and exit.
	sig_handler_destructor(sighandler);
	delete_all();

	// Note that all client threads must have terminated before you clean
	// up the database.

	pthread_mutex_lock (&(sct.server_mutex));
	while(sct.num_client_threads > 0) {
		pthread_cond_wait (&(sct.server_cond),&(sct.server_mutex));
	}
  
    db_cleanup();
	pthread_mutex_unlock (&(sct.server_mutex));
  
	pthread_cancel(listener);

    //
    // You should ensure that the thread list is empty before cleaning up the
    // database and canceling the listener thread. Think carefully about what
    // happens in a call to delete_all() and ensure that there is no way for a
    // thread to add itself to the thread list after the server's final
	return 0;
}