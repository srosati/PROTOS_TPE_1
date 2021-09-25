#include <tcpEchoAddrinfo.h>

typedef struct t_buffer
{
	char *buffer;
	size_t len;	 // longitud del buffer
	size_t from; // desde donde falta escribir
} t_buffer;

typedef struct t_client
{
	int socket;
	ptr_parser parsers[TCP_COMMANDS];
	ptr_parser end_of_line_parser;
	unsigned action;
	unsigned may_match_count;
	unsigned matched_command;
	int end_idx;
	int may_match[3];
	int read_counter;
} t_client;

static void parseSocketRead(t_client *current, char *in_buffer, t_buffer *write_buffer, int valread, fd_set *writefds);

int main(int argc, char *argv[])
{
	int opt = TRUE;
	int master_socket; // IPv4 e IPv6 (si estan habilitados)
	int new_socket, max_clients = MAX_SOCKETS, activity, curr_client, sd;

	struct t_client client_socket[MAX_SOCKETS];

	long valread;
	int max_sd;
	struct sockaddr_in address = {0};

	int port_used = PORT;

	if (argc > 1)
	{
		int received_port = atoi(argv[1]);
		log(DEBUG, "%s\n", argv[1]);

		if (received_port > 0 && received_port > MIN_PORT)
		{
			port_used = received_port;
		}
	}

	/*
	socklen_t addrlen = sizeof(address);
	struct sockaddr_storage clntAddr; // Client address
	socklen_t clntAddrLen = sizeof(clntAddr);
	*/

	char in_buffer[BUFFSIZE + 1]; //data buffer of 1K

	fd_set readfds; //set of socket descriptors

	t_buffer bufferWrite[MAX_SOCKETS]; //buffer de escritura asociado a cada socket, para no bloquear por escritura
	memset(bufferWrite, 0, sizeof bufferWrite);

	fd_set writefds;

	memset(client_socket, 0, sizeof(client_socket));

	master_socket = setupTCPServerSocket(port_used);

	int udpSock = udpSocket(PORT);
	if (udpSock < 0)
	{
		log(FATAL, "UDP socket failed");
	}
	else
	{
		log(DEBUG, "Waiting for UDP IPv4 on socket %d\n", udpSock);
	}

	struct parser_definition parser_defs[TCP_COMMANDS];
	// for (int i = 0; i < 3; i++) {
	// 	parser_defs[i] = malloc(sizeof(struct parser_definition));
	// }
	init_parser_defs(parser_defs);

	struct parser_definition end_of_line_parser_def = parser_utils_strcmpi("\r\n");

	// Limpiamos el conjunto de escritura
	FD_ZERO(&writefds);
	while (TRUE)
	{
		//clear the socket set
		FD_ZERO(&readfds);

		//add masters sockets to set
		FD_SET(master_socket, &readfds);
		FD_SET(udpSock, &readfds);

		max_sd = udpSock;

		// add child sockets to set
		for (curr_client = 0; curr_client < max_clients; curr_client++)
		{
			// socket descriptor
			sd = client_socket[curr_client].socket;

			// if valid socket descriptor then add to read list
			if (sd > 0)
			{
				FD_SET(sd, &readfds);
				max_sd = max(sd, max_sd);
			}
		}

		//wait for an activity on one of the sockets , timeout is NULL , so wait indefinitely
		activity = select(max_sd + 1, &readfds, &writefds, NULL, NULL);

		log(DEBUG, "select has something...");

		if ((activity < 0) && (errno != EINTR))
		{
			log(ERROR, "select error, errno=%d", errno);
			continue;
		}

		// Servicio UDP
		if (FD_ISSET(udpSock, &readfds))
		{
			handleAddrInfo(udpSock);
		}

		//If something happened on the TCP master socket , then its an incoming connection
		if (FD_ISSET(master_socket, &readfds))
		{
			if ((new_socket = acceptTCPConnection(master_socket)) < 0)
			{
				log(ERROR, "Accept error on master socket %d", master_socket);
				continue;
			}

			for (curr_client = 0; curr_client < max_clients; curr_client++)
			{
				if (client_socket[curr_client].socket == 0) //empty
				{
					client_socket[curr_client].action = PARSING;
					client_socket[curr_client].end_idx = -1;
					client_socket[curr_client].may_match_count = TCP_COMMANDS;
					client_socket[curr_client].matched_command = -1;
					for (int l = 0; l < TCP_COMMANDS; l++)
					{
						client_socket[curr_client].may_match[l] = 1;
					}
					client_socket[curr_client].socket = new_socket;
					init_parsers(client_socket[curr_client].parsers, parser_defs);
					client_socket[curr_client].end_of_line_parser = parser_init(parser_no_classes(), &end_of_line_parser_def);
					log(DEBUG, "Adding to list of sockets as %d\n", curr_client);
					break;
				}
			}
		}

		for (curr_client = 0; curr_client < max_clients; curr_client++)
		{
			sd = client_socket[curr_client].socket;
			if (FD_ISSET(sd, &writefds))
			{
				handleWrite(sd, bufferWrite + curr_client, &writefds);
			}
		}

		//else its some IO operation on some other socket :)
		for (curr_client = 0; curr_client < max_clients; curr_client++)
		{
			sd = client_socket[curr_client].socket;
			if (FD_ISSET(sd, &readfds))
			{
				if ((valread = read(sd, in_buffer, BUFFSIZE)) <= 0)
				{
					close(sd); //Somebody disconnected or read failed
					client_socket[curr_client].socket = 0;

					FD_CLR(sd, &writefds);
					clear(bufferWrite + curr_client);
				}
				else
				{
					parseSocketRead(&client_socket[curr_client], in_buffer, &bufferWrite[curr_client], valread, &writefds);
				}
			}
		}
	}
	return 0;
}

static void parseSocketRead(t_client *current, char *in_buffer, t_buffer *write_buffer, int valread, fd_set *writefds)
{
	int j, parse_end_idx = 0;
	for (j = 0; j < valread; j++)
	{
		if (current->read_counter == 100)
		{
			current->end_idx = j;
		}
		current->read_counter++;

		if (current->action == PARSING)
		{
			log(DEBUG, "PARSING", NULL);

			for (int k = 0; k < TCP_COMMANDS && current->matched_command == -1 && current->may_match_count > 0; k++)
			{
				if (current->may_match[k])
				{
					const struct parser_event *state = parser_feed(current->parsers[k], in_buffer[j]);
					if (state->type == STRING_CMP_EQ)
					{ //matcheo uno de los comandos (echo, date o time)
						log(DEBUG, "matched after %d bytes", j);

						parse_end_idx = j + 1;
						current->action = EXECUTING;
						current->matched_command = k;
						current->end_idx = -1;
						current->may_match_count = TCP_COMMANDS;
						current->matched_command = -1;
						for (int l = 0; l < TCP_COMMANDS; l++)
						{
							current->may_match[l] = 1;
						}
						reset_parsers(current->parsers, current->may_match);
					}
					else if (state->type == STRING_CMP_NEQ)
					{ //ya hay un comando q no matcheo
						current->may_match[k] = 0;
						current->may_match_count--;
					}
				}
			}
			// comando invalido, consumir hasta \r\n
			if (current->may_match_count == 0)
			{
				log(DEBUG, "Estoy en comando invalido");
				current->action = INVALID;
			}
		}
		else
		{
			if (!US_ASCII(in_buffer[j]) && current->end_idx == -1)
			{
				current->end_idx = j;
			}

			const struct parser_event *state = parser_feed(current->end_of_line_parser, in_buffer[j]);
			if (state->type == STRING_CMP_NEQ)
			{
				parser_reset(current->end_of_line_parser);
			}
			else if (state->type == STRING_CMP_EQ)
			{ //EOF
				if (current->action == EXECUTING)
				{
					FD_SET(current->socket, writefds);

					int end_idx = current->end_idx;
					if (current->end_idx == -1)
					{
						end_idx = j + 1;
					}
					else
					{ // hay un no usascci en este mensaje
						current->action = IDLE;
					}

					write_buffer->buffer = realloc(write_buffer->buffer, write_buffer->len + end_idx - parse_end_idx);
					memcpy(write_buffer->buffer + write_buffer->len, in_buffer + parse_end_idx, end_idx - parse_end_idx);
					write_buffer->len += end_idx - parse_end_idx;
				}

				current->end_idx = -1;
				current->action = PARSING;
				reset_parsers(current->parsers, current->may_match);
				current->matched_command = -1;
				current->may_match_count = TCP_COMMANDS;
			}
		}
	}

	if (current->action == EXECUTING)
	{
		FD_SET(current->socket, writefds);

		int end_idx = current->end_idx;
		if (current->end_idx == -1)
		{
			end_idx = j;
		}
		else
		{ // hay un no usascii en este mensaje
			current->action = IDLE;
		}

		write_buffer->buffer = realloc(write_buffer->buffer, write_buffer->len + end_idx - parse_end_idx);
		memcpy(write_buffer->buffer + write_buffer->len, in_buffer + parse_end_idx, end_idx - parse_end_idx);
		write_buffer->len += end_idx - parse_end_idx;
	}
}

void clear(t_buffer_ptr buffer)
{
	free(buffer->buffer);
	buffer->buffer = NULL;
	buffer->from = buffer->len = 0;
}

// Hay algo para escribir?
// Si está listo para escribir, escribimos. El problema es que a pesar de tener buffer para poder
// escribir, tal vez no sea suficiente. Por ejemplo podría tener 100 bytes libres en el buffer de
// salida, pero le pido que mande 1000 bytes.Por lo que tenemos que hacer un send no bloqueante,
// verificando la cantidad de bytes que pudo consumir TCP.
void handleWrite(int socket, t_buffer_ptr in_buffer, fd_set *writefds)
{
	size_t bytesToSend = in_buffer->len - in_buffer->from;
	if (bytesToSend > 0)
	{ // Puede estar listo para enviar, pero no tenemos nada para enviar
		log(INFO, "Trying to send %zu bytes to socket %d\n", bytesToSend, socket);
		size_t bytesSent = send(socket, in_buffer->buffer + in_buffer->from, bytesToSend, MSG_DONTWAIT);
		log(INFO, "Sent %zu bytes\n", bytesSent);

		if (bytesSent < 0)
		{
			// Esto no deberia pasar ya que el socket estaba listo para escritura
			// TODO: manejar el error
			log(FATAL, "Error sending to socket %d", socket);
		}
		else
		{
			size_t bytesLeft = bytesSent - bytesToSend;

			if (bytesLeft == 0)
			{
				clear(in_buffer);
				FD_CLR(socket, writefds); //ya no me interesa escribir porque ya mandé todo
			}
			else
			{
				in_buffer->from += bytesSent;
			}
		}
	}
}

int udpSocket(int port)
{

	int sock;
	struct sockaddr_in serverAddr;
	if ((sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
	{
		log(ERROR, "UDP socket creation failed, errno: %d %s", errno, strerror(errno));
		return sock;
	}
	log(DEBUG, "UDP socket %d created", sock);
	memset(&serverAddr, 0, sizeof(serverAddr));
	serverAddr.sin_family = AF_INET; // IPv4cle
	serverAddr.sin_addr.s_addr = INADDR_ANY;
	serverAddr.sin_port = htons(port);

	if (bind(sock, (const struct sockaddr *)&serverAddr, sizeof(serverAddr)) < 0)
	{
		log(ERROR, "UDP bind failed, errno: %d %s", errno, strerror(errno));
		close(sock);
		return -1;
	}
	log(DEBUG, "UDP socket bind OK ");

	return sock;
}

void handleAddrInfo(int socket)
{
	// En el datagrama viene el nombre a resolver
	// Se le devuelve la informacion asociada

	char in_buffer[BUFFSIZE];
	unsigned int len, n;

	struct sockaddr_in clntAddr;

	// Es bloqueante, deberian invocar a esta funcion solo si hay algo disponible en el socket
	n = recvfrom(socket, in_buffer, BUFFSIZE, 0, (struct sockaddr *)&clntAddr, &len);
	if (in_buffer[n - 1] == '\n') // Por si lo estan probando con netcat, en modo interactivo
		n--;
	in_buffer[n] = '\0';
	log(DEBUG, "UDP received:%s", in_buffer);
	// TODO: parsear lo recibido para obtener nombre, puerto, etc. Asumimos viene solo el nombre

	// Especificamos solo SOCK_STREAM para que no duplique las respuestas
	struct addrinfo addrCriteria;					// Criteria for address match
	memset(&addrCriteria, 0, sizeof(addrCriteria)); // Zero out structure
	addrCriteria.ai_family = AF_UNSPEC;				// Any address family
	addrCriteria.ai_socktype = SOCK_STREAM;			// Only stream sockets
	addrCriteria.ai_protocol = IPPROTO_TCP;			// Only TCP protocol

	// Armamos el datagrama con las direcciones de respuesta, separadas por \r\n
	// TODO: hacer una concatenacion segura
	// TODO: modificar la funcion printAddressInfo usada en sockets bloqueantes para que sirva
	//       tanto si se quiere obtener solo la direccion o la direccion mas el puerto
	char bufferOut[BUFFSIZE];
	bufferOut[0] = '\0';

	struct addrinfo *addrList;
	int rtnVal = getaddrinfo(in_buffer, NULL, &addrCriteria, &addrList);
	if (rtnVal != 0)
	{
		log(ERROR, "getaddrinfo() failed: %d: %s", rtnVal, gai_strerror(rtnVal));
		strcat(strcpy(bufferOut, "Can't resolve "), in_buffer);
	}
	else
	{
		for (struct addrinfo *addr = addrList; addr != NULL; addr = addr->ai_next)
		{
			struct sockaddr *address = addr->ai_addr;
			char addrBuffer[INET6_ADDRSTRLEN];

			void *numericAddress = NULL;
			switch (address->sa_family)
			{
			case AF_INET:
				numericAddress = &((struct sockaddr_in *)address)->sin_addr;
				break;
			case AF_INET6:
				numericAddress = &((struct sockaddr_in6 *)address)->sin6_addr;
				break;
			}
			if (numericAddress == NULL)
			{
				strcat(bufferOut, "[Unknown Type]");
			}
			else
			{
				// Convert binary to printable address
				if (inet_ntop(address->sa_family, numericAddress, addrBuffer, sizeof(addrBuffer)) == NULL)
					strcat(bufferOut, "[invalid address]");
				else
				{
					strcat(bufferOut, addrBuffer);
				}
			}
			strcat(bufferOut, "\r\n");
		}
		freeaddrinfo(addrList);
	}

	// Enviamos respuesta (el sendto no bloquea)
	sendto(socket, bufferOut, strlen(bufferOut), 0, (const struct sockaddr *)&clntAddr, len);

	log(DEBUG, "UDP sent:%s", bufferOut);
}

void init_parser_defs(struct parser_definition defs[TCP_COMMANDS])
{
	int i = 0;
	defs[i++] = parser_utils_strcmpi("ECHO ");
	defs[i++] = parser_utils_strcmpi("GET TIME");
	defs[i] = parser_utils_strcmpi("GET DATE");
}

void init_parsers(ptr_parser parsers[TCP_COMMANDS], struct parser_definition defs[TCP_COMMANDS])
{
	for (int i = 0; i < TCP_COMMANDS; i++)
	{
		parsers[i] = parser_init(parser_no_classes(), &defs[i]);
	}
}

void reset_parsers(ptr_parser parsers[TCP_COMMANDS], int *may_match)
{
	for (int i = 0; i < TCP_COMMANDS; i++)
	{
		parser_reset(parsers[i]);
		may_match[i] = 1;
	}
}