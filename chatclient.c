#include <stdio.h>
#include <sys/socket.h> //For Sockets
#include <stdlib.h>
#include <netinet/in.h> //For the AF_INET (Address Family)
#include <arpa/inet.h>
#include <pthread.h> // multithreading where child shares memory with parent
#include <sys/time.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <curses.h>
#include <signal.h>
#include <stdbool.h>
#include <unistd.h>

extern int errno;

#define MIN(X, Y) (((X) < (Y)) ? (X) : (Y))

#define MAX_MESSAGE_LEN 100 // the max length of a message
#define MAX_LINE_LEN 150 // the max length of a line
#define MAX_LINES_SAVED 100 // the number of lines to save to memory in case they are no longer displayed on the window

#define BACKSPACE 127 // numerical value of backspace character
#define ARROW_UP 65
#define ARROW_DOWN 66
#define ARROW_RIGHT 67
#define ARROW_LEFT 68
#define NEWLINE 10

WINDOW *window; // curses window

int lastLines; // used to check if the number of lines changed when resizing the window
int savedLinesCount = 0; // the number of lines currently saved in memory
char * savedLines; // pointer to the lines saved in memory

pthread_mutex_t lock;

struct sockaddr_in address;
int addrlen;
int mainsocket;
char typedbuffer[MAX_MESSAGE_LEN + 1]; // contains the string the user has currently typed in plus a null terminator
uint8_t running;
int cursorPosX;

/*
	Signal handler used to wake receiving thread from sleep and tell it to stop running
*/
void stopRunning(int signum){
	running = 0;
}

/*
	Send a message to the server
*/
int sendall(int socket, char * str, size_t len, int flags){

	int sent = 0, n;

	while(sent < len){
		n = send(socket, str + sent, len - sent, flags);
		if(n == -1) break; // error
		if(n == 0) break;
		sent += n;
	}
	
	return n == -1 ? -1 : 0;
}

/*
	Receive a message from the server
*/
int recvall(int socket, char * buffer, size_t len, int flags){
	int received = 0, n;

	memset(buffer, '\0', 1);

	while(received < len){
		n = recv(socket, buffer + received, len - received, flags);
		if(n == -1) break; // error
		if(n == 0) break;
		received += n;
	}

	return n == -1 ? -1 : 0;
}

/*
	Print a line and save it to history
*/
void printAndSaveLine(char* line, bool locked){
	
	if(locked) pthread_mutex_lock(&lock); // start of critical section

	move(LINES - 1, 0);
	printw("%s\n", line); // print typed text
	move(LINES - 1, cursorPosX);
	refresh();

	// Allocate memory for a line
	if(savedLinesCount == 0) savedLines = malloc(MAX_LINE_LEN);
	else if(savedLinesCount < MAX_LINES_SAVED) savedLines = realloc(savedLines, (savedLinesCount + 1) * MAX_LINE_LEN);
	else{
		memmove(savedLines, savedLines + MAX_LINE_LEN, (MAX_LINES_SAVED - 1) * MAX_LINE_LEN);
		memmove(savedLines + ((MAX_LINES_SAVED - 1) * MAX_LINE_LEN), line, MAX_LINE_LEN);
		return;
	}

	memmove(savedLines + (savedLinesCount * MAX_LINE_LEN), line, MAX_LINE_LEN); // store line in memory

	savedLinesCount++;

		
	if(locked) pthread_mutex_unlock(&lock); // end of critical section
}	

/*
	Command that prints your entire chat history
*/
void command_chatHistory(){

	pthread_mutex_lock(&lock); // start of critical section

	int initSavedLinesCount = savedLinesCount;

	move(LINES - 1, 0);
	printAndSaveLine("\nChat History:\n", false);
	for(int i = 0; i < initSavedLinesCount; i++){
		printAndSaveLine(savedLines + (i * MAX_LINE_LEN), false);
	}
	printAndSaveLine("", false);

	pthread_mutex_unlock(&lock); // end of critical section
}

/*
	Executed whenever the window is resized; Clears and redraws the window so everything is in the right place
*/
void windowResize(){

	if(lastLines != LINES){ // if number of lines changed, redraw everything on the screen		
		
		clear();

		int linesToPrint = MIN(LINES - 1, savedLinesCount);
		for(int i = 0; i < linesToPrint; i++){
			char line[MAX_LINE_LEN];
			memmove(line, savedLines + (savedLinesCount - 1 - i) * MAX_LINE_LEN, MAX_LINE_LEN);
			move(LINES - 2 - i, 0);	
			printw("%s", line);
		}
		lastLines = LINES;
	}
	move(LINES - 1, cursorPosX);
	refresh();
}

/*
	Function executed by receiving thread; Receives messages from the server and prints them
*/
void *receiveMessages(void* vargs){

	signal(SIGUSR1, stopRunning);
	while(running != 0){
		
		char recvbuffer[MAX_MESSAGE_LEN];

        	int valread = recvall(mainsocket, recvbuffer, MAX_MESSAGE_LEN, 0);

		if(recvbuffer[0] == '\0'){
			if(running == 0) return NULL;
			move(LINES - 1, 0);
			printAndSaveLine("\nConnection lost.\n", true);
			close(mainsocket);

			mainsocket = socket(AF_INET, SOCK_STREAM, 0); // attempt to initialize new socket
			if(mainsocket < 0){
				printw("Socket creation failed.\n");
				return NULL;
			}
			while(connect(mainsocket, (struct sockaddr *)&address, addrlen) < 0){
				printAndSaveLine("\nRetrying in\n", true);
				refresh();
				for(int i = 5; i > 0; i--){
					char num[MAX_LINE_LEN];
					sprintf(num, "%d", i);
					printAndSaveLine(num, true);

					printw("%s", typedbuffer);
					move(LINES - 1, cursorPosX);
					refresh();

					sleep(1);
					if(running == 0) return NULL;
				}
			}
			printAndSaveLine("\nReconnection succeeded.\n", true);
			continue;
		}

		printAndSaveLine(recvbuffer, true);

		move(LINES - 1, 0);
		printw("%s", typedbuffer); // reprint typed text
		move(LINES - 1, cursorPosX);

		refresh();
	}
	return NULL;
}

/*
	Main thread; initializes curses window and receiving thread, reads user input, and sends messages to the server
*/
int main(int argc, char* argv[]){
	
	address.sin_family = AF_INET;
	address.sin_port = htons(8096);

	// Convert IPv4 and IPv6 addresses from text to binary form 
        if(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr)<=0)  
        { 
                printf("\nInvalid address/ Address not supported \n"); 
                return -1; 
        }

	addrlen = sizeof(address);
	mainsocket = socket(AF_INET, SOCK_STREAM, 0); // attempt to initialize socket

	if(mainsocket < 0){
		perror("Socket creation failed.\n");
		return -1;
	}

	if (connect(mainsocket, (struct sockaddr *)&address, addrlen) < 0) 
    	{ 
        	printf("\nConnection Failed \n");
        	return -1;
    	}

	if (pthread_mutex_init(&lock, NULL) != 0){ // initialize the mutex lock
        	printf("mutex init failed\n");
        	return -1;
    	}

	int c; // holds a character receive from a getch() call

	// data shared between the main and receiving threads
	mainsocket = mainsocket;
	running = 1;
	cursorPosX = 0;

	window = initscr(); // initialize the curses window
	scrollok(window, TRUE); // enable scrolling of text
	move(LINES - 1, 0); // move cursor to bottom of screen
	noecho(); // disable the echoing of typed characters, we will print them ourselves

	lastLines = LINES; // keep record of how many lines there are for if the window resizes
	int y, x;

	pthread_t receiveThread;
	pthread_create(&receiveThread, NULL, &receiveMessages, NULL); // start thread for receiving messages

	// for testing the getch values of certain keys
	/*
	c = getch();
	printw("%d\n", c);
	refresh();
	c = getch();
	printw("%d\n", c);
	refresh();
	c = getch();
	printw("%d\n", c);
	refresh();
	c = getch();
	printw("%d\n", c);
	refresh();
	sleep(5);
	endwin();
	return 0;
	*/

	typedbuffer[0] = '\0';

	while(true){
		c = getch();
		switch(c){
			case KEY_RESIZE: // resize window
				windowResize();
				break;
			case BACKSPACE: // delete one character from the buffer
				getyx(window, y, x);
				if(x <= 0) break;
				memmove(typedbuffer+(x-1), typedbuffer+x, strlen(typedbuffer)-(x-1));
				cursorPosX = x-1;
				break;
			case 27:
				if(getch() == 91){
					c = getch();
					switch(c){
						case ARROW_LEFT:
							getyx(window, y, x);
							if(x <= 0) break;
							cursorPosX = x-1;
							break;
						case ARROW_RIGHT:
							getyx(window, y, x);
							if(x >= strlen(typedbuffer)) break;
							cursorPosX = x+1;
							break;
						case 51:
							c = getch();
							switch(c){
								case 126: // delete key pressed
									getyx(window, y, x);
									if(x >= MAX_MESSAGE_LEN) break;
									memmove(typedbuffer+x, typedbuffer+(x+1), strlen(typedbuffer)-x);
									break;
							}
					}
				}
				break;
			case NEWLINE: // don't save newline to buffer
				break;
			default: // add one character to the buffer
				getyx(window, y, x);
				if(x >= MAX_MESSAGE_LEN || strlen(typedbuffer) >= MAX_MESSAGE_LEN) break;
				memmove(typedbuffer+(x+1), typedbuffer+x, strlen(typedbuffer)-(x-1));
				cursorPosX = x+1;
				typedbuffer[x] = (char)c;
				break;
		}


		if(c == NEWLINE) {

			printAndSaveLine(typedbuffer, true);
			cursorPosX = 0;

			move(LINES - 1, 0);

			// commands

			if(strncmp(typedbuffer, "/hist", 5) == 0) command_chatHistory();

			else if(strncmp(typedbuffer, "/exit", 5) == 0) break;
			
			else sendall(mainsocket, typedbuffer, MAX_MESSAGE_LEN, 0);

			typedbuffer[0] = '\0';
			c = NULL;

			continue;
		}

		move(LINES - 1, 0); // move cursor to bottom of screen

		clrtoeol(); // clear bottom row
		printw("%s", typedbuffer); // print typed text
		
		move(LINES - 1, cursorPosX);
		refresh();

	}
	shutdown(mainsocket, 2);
	pthread_kill(receiveThread, SIGUSR1);
	pthread_join(receiveThread, NULL);
	pthread_mutex_destroy(&lock);
	free(savedLines);
	endwin();
    	return 0;
}
