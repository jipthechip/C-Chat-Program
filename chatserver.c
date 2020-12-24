#include <stdio.h>
#include <sys/socket.h> //For Sockets
#include <stdlib.h>
#include <netinet/in.h> //For the AF_INET (Address Family)
#include <arpa/inet.h>
#include <pthread.h> // multithreading where child shares memory with parent
#include <sys/time.h>
#include <string.h>
#include <stdbool.h>
#include <signal.h>

#define MAX_CLIENTS 30
#define MAX_MESSAGE_LEN 100
#define USER_LEN 51
#define PASSWORD_LEN 51
#define USER_AND_PASSWORD_LEN 102 // the length of each username/password line kept on file plus null terminators

int clientsocket[MAX_CLIENTS];
int clientstate[MAX_CLIENTS];
char accountinfo[MAX_CLIENTS * (USER_AND_PASSWORD_LEN)]; // contains account info for each user

void writeAccount(FILE * fp, char * account){
	fseek(fp, 0, SEEK_END);
	fwrite(account, USER_AND_PASSWORD_LEN, 1, fp);
	fwrite("\n", 1, 1, fp);
}

bool userLoggedIn(char * user){
	for(int i = 0; i < MAX_CLIENTS; i++){
		printf("Checking %s against %s\n", user, accountinfo + (USER_AND_PASSWORD_LEN * i));	
		if(strncmp(user, accountinfo + (USER_AND_PASSWORD_LEN * i), strlen(accountinfo + (USER_AND_PASSWORD_LEN * i))) == 0 && strlen(user) == strlen(accountinfo + (USER_AND_PASSWORD_LEN * i))){		
			return true;
		}
	}
	return false;
}

bool fileContainsUser(FILE * fp, char * user, char * line){
	rewind(fp);
	char buffer[USER_AND_PASSWORD_LEN];
	int scanval = 1;
	while(scanval != EOF && scanval != 0){
		char format[16];
		sprintf(format, "%%%zu[^\n]\n", USER_AND_PASSWORD_LEN+1);
		scanval = fscanf(fp, format, buffer);
		if(scanval != 0){
			if(strncmp(buffer, user, strlen(buffer)) == 0){
				memcpy(line, buffer, USER_AND_PASSWORD_LEN);
				return true;
			}
		}
	}
	return false;
}
/*
	Send a message to the client
*/
int sendall(int socket, char * buffer, size_t len, int flags){

	int sent = 0, n;

	while(sent < len){
		n = send(socket, buffer + sent, len - sent, flags);
		if(n == -1) break; // error
		sent += n;
	}

	printf("Sent '%.*s'\n", sent, buffer);

	return n == -1 ? -1 : sent;
}

/*
	Receive a message from the client
*/
int recvall(int socket, char * buffer, size_t len, int flags){

	int received = 0, n;

	while(received < len){
		n = recv(socket, buffer + received, len - received, flags);
		if(n == -1) break; // error
		if(n == 0) break;
		received += n;
	}
	
	printf("Received '%.*s' from socket %d\n", received, buffer, socket);

	return n == -1 ? -1 : received;
}

void sendToAllUsers(char * msg, int sd, bool selfsend, int user){
	if(user != -1){
		char username[USER_LEN + 2];
		strcpy(username, accountinfo + (USER_AND_PASSWORD_LEN * user));
		strcat(username, ": ");
		printf("Username is %s\n", username);
		memmove(msg + strlen(username), msg, strlen(msg));
		memcpy(msg, username, strlen(username));
	}
	for(int j = 0; j < MAX_CLIENTS; j++){
		if(clientsocket[j] == sd && !(selfsend)) continue;
		else if(clientsocket[j] > 0 && clientstate[j] == 4){
			sendall(clientsocket[j], msg, MAX_MESSAGE_LEN, 0); // send message to other users
		}
	}	
	memset(msg, 0, sizeof(msg));
}

int main(int argc, char* argv[]){

	sigaction(SIGPIPE, &(struct sigaction){SIG_IGN}, NULL);

	struct sockaddr_in address; //This is our main socket variable.
	int mastersocket; //This is the socket file descriptor that will be used to identify the socket

	address.sin_family = AF_INET;
	address.sin_port = htons(8096); //Define the port at which the server will listen for connections.
	address.sin_addr.s_addr = INADDR_ANY;

	int addrlen = sizeof(address);

	mastersocket = socket(AF_INET, SOCK_STREAM, 0); //This will create a new socket and also return the 
					      		//identifier of the socket into fd
	
	int opt = 1;
	//set master socket to allow multiple connections , this is just a good habit, it will work without this
    	if( setsockopt(mastersocket, SOL_SOCKET, SO_REUSEADDR, (char *)&opt, sizeof(opt)) < 0 )
    	{
        	perror("setsockopt\n");
        	exit(EXIT_FAILURE);
    	}
	
	bind(mastersocket, (struct sockaddr *)&address, addrlen); //assigns the address specified by serv to the socket

	fd_set readfds;

	listen(mastersocket, MAX_CLIENTS);
	
	int activity;
	int new_socket;

	// open username and password file
	FILE* accounts = fopen("accounts.txt", "a+");

	while(1){
		FD_ZERO(&readfds);
		FD_SET(mastersocket, &readfds);

		int sd;
		int maxsd = mastersocket;
		for(int i = 0; i < MAX_CLIENTS; i++){
			sd = clientsocket[i];
			if(sd > 0){
				FD_SET(sd, &readfds);
			}
			if(sd > maxsd){
				maxsd = sd;
			}
		}

		activity = select(maxsd+1, &readfds, NULL, NULL, NULL);
		
		// handle new connection
		if(FD_ISSET(mastersocket, &readfds)){
			if((new_socket = accept(mastersocket, (struct sockaddr *)&address, (socklen_t *)&addrlen)) < 0){
				perror("error accepting new client connection.\n");
			}
			printf("New connection, socket fd is %d , ip is : %s , port : %d \n", new_socket, inet_ntoa(address.sin_addr), ntohs(address.sin_port));

			char welcomemessage[MAX_MESSAGE_LEN];
			strcpy(welcomemessage, "Welcome, please enter your username.");

			if(sendall(new_socket, welcomemessage, MAX_MESSAGE_LEN, 0) != MAX_MESSAGE_LEN){
				perror("Error sending welcome message.\n");
			}

			for(int i = 0; i < MAX_CLIENTS; i++){
				if(clientsocket[i] == 0){
					clientsocket[i] = new_socket;
					printf("New socket in slot %d.\n", i);
					break;
				}
			}
		}
		
		size_t messagelen;
		char buffer[MAX_MESSAGE_LEN];

		for(int i = 0; i < MAX_CLIENTS; i++){
			sd = clientsocket[i];
			if(FD_ISSET(sd, &readfds)){
				printf("Activity on socket %d\n", i);

				// client disconnected, close socket
				if(recvall(clientsocket[i], buffer, MAX_MESSAGE_LEN, 0) == 0){
					//Somebody disconnected , get their details and print
                    			getpeername(sd, (struct sockaddr*)&address , (socklen_t*)&addrlen);
                    			printf("Host disconnected, ip %s , port %d \n" , inet_ntoa(address.sin_addr) , ntohs(address.sin_port));
					char exitmsg[MAX_MESSAGE_LEN];
					sprintf(exitmsg, "%s left the server.", accountinfo + (USER_AND_PASSWORD_LEN * i));
					if(clientstate[i] == 4) sendToAllUsers(exitmsg, sd, false, -1);
					close(sd);
					clientsocket[i] = 0;
					clientstate[i] = 0;
					memset(accountinfo + (USER_AND_PASSWORD_LEN * i), 0, USER_AND_PASSWORD_LEN);
				}
				// client is sending message
				else{
					char passwordprompt[MAX_MESSAGE_LEN];
					char line[USER_AND_PASSWORD_LEN];
					char welcomemessage[MAX_MESSAGE_LEN];
					switch(clientstate[i]){
						case 0: // client has not entered username yet
							if(userLoggedIn(buffer)){
								sprintf(passwordprompt, "%s is already logged in! Try another username:", buffer);
								sendall(sd, passwordprompt, MAX_MESSAGE_LEN, 0);
							}							
							else if(fileContainsUser(accounts, buffer, line)){
								strcpy(passwordprompt, "Please enter your password:");
								sendall(sd, passwordprompt, MAX_MESSAGE_LEN, 0);
								memcpy(accountinfo + (USER_AND_PASSWORD_LEN * i), line, USER_AND_PASSWORD_LEN);
								clientstate[i] = 1;
							}
							else {
								strcpy(passwordprompt, "Account does not exist. Type a password to create a new account or /back to go back:");
								sendall(sd, passwordprompt, MAX_MESSAGE_LEN, 0);
								strcpy(accountinfo + (USER_AND_PASSWORD_LEN * i), buffer);
								clientstate[i] = 2;
							}
							break;
						case 1: // client has entered existing username, but needs to enter password
							if(strncmp(buffer, "/back", strlen(buffer)) == 0 && strlen(buffer) == strlen("/back")){ // go back to entering username
								clientstate[i] = 0;
								strcpy(welcomemessage, "Welcome, please enter your username.");
								sendall(sd, welcomemessage, MAX_MESSAGE_LEN, 0);
							}else if(strncmp(buffer, accountinfo + (USER_AND_PASSWORD_LEN * i) + USER_LEN, strlen(buffer)) == 0 && strlen(buffer) == strlen(accountinfo + 								(USER_AND_PASSWORD_LEN * i) + USER_LEN)){ // client logs in successfully
								sprintf(passwordprompt, "%s logged in successfully.", accountinfo + (USER_AND_PASSWORD_LEN * i));
								clientstate[i] = 4;
								sendToAllUsers(passwordprompt, sd, true, -1);
							}else{ // password incorrect
								strcpy(passwordprompt, "Incorrect password. Try again or type /back to go back:");
								sendall(sd, passwordprompt, MAX_MESSAGE_LEN, 0);
							}
							break;
						case 2: // client has entered non-existing username, needs to create password
							if(strncmp(buffer, "/back", strlen(buffer)) == 0 && strlen(buffer) == strlen("/back")){ // go back to entering username
								clientstate[i] = 0;
								strcpy(welcomemessage, "Welcome, please enter your username.");
								sendall(sd, welcomemessage, MAX_MESSAGE_LEN, 0);
							}else{ // entered password
								strcpy(accountinfo + (USER_AND_PASSWORD_LEN * i) + USER_LEN, buffer);
								strcpy(passwordprompt, "Reenter your password or type /back to go back:");
								sendall(sd, passwordprompt, MAX_MESSAGE_LEN, 0);
								clientstate[i] = 3;
							}
							break;
						case 3: // client entered new password once, needs to reenter
							if(strncmp(buffer, "/back", strlen(buffer)) == 0 && strlen(buffer) == strlen("/back")){ // go back to entering username
								clientstate[i] = 0;
								strcpy(welcomemessage, "Welcome, please enter your username.");
								sendall(sd, welcomemessage, MAX_MESSAGE_LEN, 0);
							}else if(strncmp(buffer, accountinfo + (USER_AND_PASSWORD_LEN * i) + USER_LEN, strlen(buffer)) == 0 && strlen(buffer) == strlen(accountinfo + 									(USER_AND_PASSWORD_LEN * i) + USER_LEN)){ // password
								sprintf(passwordprompt, "%s logged in successfully.", accountinfo + (USER_AND_PASSWORD_LEN * i));
								
								writeAccount(accounts, accountinfo + (USER_AND_PASSWORD_LEN * i));								

								clientstate[i] = 4;
								sendToAllUsers(passwordprompt, sd, true, -1);
							}else{
								strcpy(passwordprompt, "Passwords don't match. Try again or type /back to go back:");
								sendall(sd, passwordprompt, MAX_MESSAGE_LEN, 0);
							}
							break;
						case 4: // client is logged in
							if(strncmp(buffer, "/back", strlen(buffer)) == 0 && strlen(buffer) == strlen("/back")){ // go back to entering username
								clientstate[i] = 0;
							}
							else{
								sendToAllUsers(buffer, sd, false, i);
							}
							break;
					}
				}
			}
		}
	}
	printf("Broke out of main loop\n");
}
