#ifdef WINDOWS
#include <windows.h>
#include <io.h>
#include <conio.h>
#include <direct.h>
#else
#	include <unistd.h>
#	include <pthread.h>
#       include <dirent.h>
#ifndef DARWIN
#	include <malloc.h>
#endif //DARWIN
#endif

#include <stdio.h>
#include <sstream>
#include "defs.h"
#include "filefuncs.h"

#include <ctime>
#ifndef UNIX  	// Win32...
#	define THREAD_RETURN_FALSE false
#	define THREAD_RETURN_TRUE true
typedef bool threadRoutineType;
typedef int threadRoutineArgType;
#else		// UNIX...
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>

#ifndef PATH_MAX
#   define PATH_MAX FILENAME_MAX
#endif
typedef void * threadRoutineType;
typedef void * threadRoutineArgType;
#endif

#include "process.h"
#include "vardb.h"
#include "engine.h"
#include "socket.hpp"

#define THREAD_RETURN_FALSE ((void *)NULL)
#define THREAD_RETURN_TRUE ((void *)1)
#define closeHandle(h)

static bool noThread = false;					// debug non-threaded

#ifndef WINDOWS
void sleep(int h)
{
	struct timespec t;
	if (noThread) return;
	t.tv_sec = (h / 1000);
	t.tv_nsec = (h % 1000) * 1000000;
	nanosleep(&t, NULL);
}
#else
#include <windows.h>
#define sleep Sleep
#define createThread CreateThread
#endif

extern int 	TCPIPTimeOut;
extern char exePath[500];

#ifdef WINDOWS
bool	WINAPI friendEngineHelper(friendEngine	*);
#else
threadRoutineType	friendEngineHelper(threadRoutineArgType);
#endif

// generates a unique id for a session, based on time
void generateSessionID(char *id, size_t maxSize)
{
	char timeString[200];
	time_t timeVar;
	struct tm *timeInfo;

	time(&timeVar);
	timeInfo = localtime(&timeVar);
	strftime(timeString, maxSize, "%Y%m%d%H%M%S", timeInfo);
	sprintf(id, "%s%d", timeString, rand() % 100 + 1);
}

// This is the entry point function. It starts a server that listen in `port` port and calls a function to handle a FRONTEND request (in a threading way or not, depending on variable noThread)
bool	friendEngine::server(BYTE	*port)
{
	Socket		MSocket;
#ifdef WINDOWS
	DWORD		threadID;
	HANDLE		threadHandle;
#else
	pthread_t 	threadID;
#endif
	MSocket.SetTimeOut(TCPIPTimeOut);

	if (!MSocket.Listen((char*)port))
	{
		fprintf(stderr, "***FRIEND Engine failed to start - bind error\n");
		return (false);	// failed on the bind
	}
	else fprintf(stderr, "FRIEND Engine sucessfully started on port %s\n", (char*)port);

	while (MSocket.Accept())
	{

		while (lock)
			sleep(50);
		lock = 1;

		childSocketFd = MSocket.Socketfd;
		if (noThread) friendEngineHelper(this);
		else
		{
#ifdef WINDOWS
			if (!(threadHandle = createThread(NULL, 0x0000f000, (LPTHREAD_START_ROUTINE)friendEngineHelper, this, 0, &threadID)))
#else	// UNIX
			pthread_create(&threadID, NULL, friendEngineHelper, (void *)this);
			pthread_detach(threadID);
			if (!threadID)
#endif
			{
				fprintf(stderr, "***Failed to create thread for child\n");
				closesocket(MSocket.Socketfd);
			}
			else closeHandle(threadHandle);
		}

		MSocket.Socketfd = 0;
		MSocket.Connected = false;

	}
#ifdef	DEBUG_MODE
	fprintf(stderr, "***Error in Accept() function call\n");
	fprintf(stderr, "***Attemping to re-bind socket\n");
#endif
	return (false);
}

// just a function to start a thread
bool	friendEngine::serverChildThread(int	socketFd)
{
	serverChild(socketFd);
	return (false);
}

// here the action actually happens
bool	friendEngine::serverChild(int	socketFd)
{
	time_t iniTime, endTime;
	LogObject *logObject = new LogObject;


	char buffer[BUFF_SIZE];
	char configFile[BUFF_SIZE] = {};
	FriendProcess process;
	char command[255], tag[255], value[255];
	char sessionID[200];
	Socket2 socks;
	socks.setSocketfd(socketFd);
	socks.setTimeOut(TCPIPTimeOut);
	socks.setBufferSize(1024 * 1024);


	iniTime = time(NULL);

	logObject->writeLog(1, "Current dir = %s\n", workingDir);
	if (strlen(workingDir) < 2)
	{
		getcwd(workingDir, 500);
	}

	logObject->writeLog(1, "Current dir = %s\n", workingDir);
	sprintf(configFile, "%s%c%s", workingDir, PATHSEPCHAR, "study_params.txt");
	strcpy(exePath, workingDir);

	// setting the logObject 
	process.setLogObject(logObject);
	// setting the response socket
	process.setSocketfd(socketFd);
	// setting the library path to the default directory
	process.setLibraryPath(workingDir);
	// setting the engine executable path
	process.setApplicationPath(workingDir);
	// begining things
	process.initializeStates();

	if (workingDir != NULL)
	{
		// reading commands from FRONTEND until END or ENDSESSION shows up
		while (1)
		{
			// reading the command
			logObject->writeLog(1, "waiting for command \n");
			socks.readLine(command, 255);
			stripReturns(command);

			if ((strlen(command) == 0) && (socks.connectionProblem)) // possible transmission error, timeout communication
			{
				logObject->writeLog(1, "Connection problems. Aborting thread!\n");
				break;
			}
			else logObject->writeLog(1, "command received : %s\n", command);
			strToUpper(command);

			// non blocked commands
			if (strcmp(command, "NEWSESSION") == 0)
			{
				generateSessionID(sessionID, 200);
				Session *newSession = new Session();

				sessionList[string(sessionID)] = newSession;

				sprintf(command, "%s\n", sessionID);
				socks.writeString(command);

				sprintf(command, "OK\n");
				socks.writeString(command);

				// assigning the session variable
				process.setSessionPointer(newSession);

				// reading the config file
				process.readConfigFile(configFile);
				char sessionLogFileName[500];
				sprintf(sessionLogFileName, "%s%c%s.txt", workingDir, PATHSEPCHAR, sessionID);
				logObject->writeLog(1, "Session %s created.\n", sessionID);
				//freopen(sessionLogFileName, "w+", stderr);
			}
			else
				// sends the sessionId of the last opended session
				if (strcmp(command, "LASTSESSION") == 0)
				{
					if (sessionList.size() > 0)
					{
						std::map<string, Session *>::reverse_iterator it;
						it = sessionList.rbegin();
						strcpy(sessionID, it->first.c_str());
					}
					else strcpy(sessionID, "NOTFOUND");

					sprintf(command, "%s\n", sessionID);
					socks.writeString(command);

					sprintf(command, "OK\n");
					socks.writeString(command);
				}
				else
					if (strcmp(command, "ENDSESSION") == 0)
					{
						// reading sessionID
						socks.readLine(sessionID, 200);
						stripReturns(sessionID);

						// getting the session pointer. Have to replace for a function for thread reasons
						std::map<string, Session *>::iterator it;
						it = sessionList.find(string(sessionID));

						// have to replace it for a function for thread reasons
						process.setSessionPointer(NULL);
						if (it != sessionList.end())
						{
							Session *session = sessionList[string(sessionID)];

							delete session;
							sessionList.erase(it);
							logObject->writeLog(1, "Session %s deleted.\n", sessionID);
						}

						logObject->writeLog(1, "Sending last ack.\n");
						sprintf(command, "OK\n");
						socks.writeString(command);

						process.wrapUpRun();
						logObject->writeLog(1, "Process object finalized.\n");
						fflush(stderr);
						logObject->closeLogFile();
						break;
					}
					else
						if (strcmp(command, "STOPSESSION") == 0)
						{
							// reading sessionID
							socks.readLine(sessionID, 200);
							stripReturns(sessionID);

							// getting the session pointer. Have to replace for a function for thread reasons
							std::map<string, Session *>::iterator it;
							it = sessionList.find(string(sessionID));

							if (it != sessionList.end())
							{
								Session *session = sessionList[string(sessionID)];
								session->terminate();
								logObject->writeLog(1, "Session %s is stopping.\n", sessionID);
							}
							else  logObject->writeLog(1, "Error occured while stopping session %s.\n", sessionID);

							logObject->writeLog(1, "Sending ack.\n");
							sprintf(command, "OK\n");
							socks.writeString(command);
							break;
						}
						else
							if (strcmp(command, "SESSION") == 0)
							{
								logObject->writeLog(1, "Waiting for session id.\n");

								// reading sessionID
								socks.readLine(sessionID, 200);
								stripReturns(sessionID);

								logObject->writeLog(1, "receiving session id %s.\n", sessionID);

								// getting the session pointer
								std::map<string, Session *>::iterator it;
								it = sessionList.find(string(sessionID));
								if (it != sessionList.end())
								{
									Session *session = sessionList[string(sessionID)];
									char response[500];

									// good acknowledge
									sprintf(command, "OK\n");
									socks.writeString(command);

									// parsing the non blocked commands
									socks.readLine(command, 255);
									stripReturns(command);
									logObject->writeLog(1, "subcommand received : %s\n", command);

									// returns the confound parameters for display
									if (strcmp(command, "GRAPHPARS") == 0)
									{
										int index = 0;
										char number[30];
										studyParams *vdbPtr = (studyParams *)session->getVDBPointer();

										socks.readLine(number, 10);
										stripReturns(number);
										index = atoi(number);
										logObject->writeLog(1, "volume index : %d\n", index);

										if (index > vdbPtr->runSize)
											strcpy(response, "END\n");
										else
											session->getGraphResponse(index, response);

										socks.writeString(response);
										logObject->writeLog(1, "response : %s\n", response);

										sprintf(command, "OK\n");
										socks.writeString(command);
									}
									else
										// returns the feedback response
										if (strcmp(command, "TEST") == 0)
										{
											int index = 0;
											char number[30];

											socks.readLine(number, 10);
											stripReturns(number);
											index = atoi(number);

											session->getFeedbackResponse(index, response);
											socks.writeString(response);
											logObject->writeLog(1, "Volume index %d\n", index);
											logObject->writeLog(1, "Sending response %s\n", response);

											sprintf(command, "OK\n");
											socks.writeString(command);
										}
										else
										{
											session->getCommandResponse(string(command), response);
											logObject->writeLog(1, "Sending response %s", response);
											socks.writeString(response);
										}
								}
								else
								{
									// bad acknowledge. Session not found
									logObject->writeLog(1, "Session not found %s\n", sessionID);
									sprintf(command, "NOK\n");
									socks.writeString(command);
								}
								break;
							}
							else
								// executing GLM. Possibly this with turn in a plug-in function
								if (strcmp(command, "GLM") == 0)
								{
									logObject->writeLog(1, "Executing Glm.\n");
									process.prepRealtimeVars();
									process.glm();

									sprintf(command, "OK\n");
									socks.writeString(command);
								}
								else
									// executing GLM. Possibly this with turn in a plug-in function
									if (strcmp(command, "NBGLM") == 0)
									{
										process.setPhaseStatus("GLM", 0);

										// sending ack
										sprintf(command, "OK\n");
										socks.writeString(command);

										// executing glm
										logObject->writeLog(1, "Executing Glm.\n");
										process.prepRealtimeVars();
										process.glm();

										process.setPhaseStatus("GLM", 1);
									}
									else
										// executing training. Here if there isn't a function defined through a PLUGIN call, nothing happens
										if (strcmp(command, "TRAIN") == 0)
										{
											process.prepRealtimeVars();
											logObject->writeLog(1, "Executing TRAIN function.\n");
											process.train();

											sprintf(command, "OK\n");
											socks.writeString(command);
										}
										else
											// executing training. Here if there isn't a function defined through a PLUGIN call, nothing happens
											if (strcmp(command, "NBTRAIN") == 0)
											{
												process.setPhaseStatus("TRAIN", 0);
												process.prepRealtimeVars();
												logObject->writeLog(1, "Executing TRAIN function.\n");
												process.train();

												process.setPhaseStatus("TRAIN", 1);
											}
											else
												// executing testing. Here if there isn't a function defined through a PLUGIN call, nothing happens
												if (strcmp(command, "TEST") == 0)
												{
													int index = 0;
													char number[30];
													float classNum, projection;

													logObject->writeLog(1, "Executing test function.\n");
													socks.readLine(number, 10);
													stripReturns(number);
													index = atoi(number);
													logObject->writeLog(1, "index received = %d\n", index);

													process.test(index, classNum, projection);

													sprintf(command, "%f\n%f\n", classNum, projection);
													socks.writeString(command);

													sprintf(command, "OK\n");
													socks.writeString(command);
												}
												else
													// executing the feature selection step. In future, we will turn this in a plug-in function
													if (strcmp(command, "FEATURESELECTION") == 0)
													{
														logObject->writeLog(1, "Executing Feature Selection.\n");
														process.featureSelection();

														sprintf(command, "OK\n");
														socks.writeString(command);
													}
													else
														// executing the feature selection step. In future, we will turn this in a plug-in function
														if (strcmp(command, "NBFEATURESELECTION") == 0)
														{
															process.setPhaseStatus("FEATURESELECTION", 0);

															// sending ack
															sprintf(command, "OK\n");
															socks.writeString(command);

															// running feature selection
															logObject->writeLog(1, "Executing Feature Selection.\n");
															process.featureSelection();

															process.setPhaseStatus("FEATURESELECTION", 1);
														}
														else
															// preproc FRIEND operations
															if (strcmp(command, "PREPROC") == 0)
															{
																logObject->writeLog(1, "PrepRealtime.\n");
																if (!process.prepRealTime())
																{
																	process.wrapUpRun();
																	sprintf(command, "NOK\n");
																	logObject->writeLog(1, "Terminating the session.\n");
																	socks.writeString(command);
																	//exit(-1);
																	break;
																}
																else
																{
																	logObject->writeLog(1, "PrepRealtime step finished successfully.\n");
																	sprintf(command, "OK\n");
																	socks.writeString(command);
																}
															}
															else
																// preproc FRIEND operations
																if (strcmp(command, "NBPREPROC") == 0)
																{
																	process.setPhaseStatus("PREPROC", 0);

																	// sending ack
																	sprintf(command, "OK\n");
																	socks.writeString(command);

																	// preprocessing
																	logObject->writeLog(1, "PrepRealtime.\n");
																	if (!process.prepRealTime())
																	{
																		process.wrapUpRun();
																		sprintf(command, "NOK\n");
																		logObject->writeLog(1, "Terminating the session.\n");
																		socks.writeString(command);
																		//exit(-1);
																		break;
																	}
																	else
																	{
																		logObject->writeLog(1, "PrepRealtime step finisjed succesfully.\n");
																		sprintf(command, "OK\n");
																		socks.writeString(command);
																	}
																	process.setPhaseStatus("PREPROC", 1);
																}
																else
																	// pipeline function. This is the only function that runs until then end of processing all volumes in the run
																	if (strcmp(command, "PIPELINE") == 0)
																	{
																		process.runRealtimePipeline();
																		sprintf(command, "OK\n");
																		socks.writeString(command);
																	}
																	else
																		// pipeline function. This is the only function that runs until then end of processing all volumes in the run
																		if (strcmp(command, "NBPIPELINE") == 0)
																		{
																			process.setPhaseStatus("PIPELINE", 0);

																			// sending ack
																			sprintf(command, "OK\n");
																			socks.writeString(command);

																			// processing pipeline
																			process.runRealtimePipeline();
																			process.setPhaseStatus("PIPELINE", 1);
																		}
																		else
																			// pipeline function. This is the only function that runs until then end of processing all volumes in the run. Here we set automatic feedback calculations. Needs an open session to work.
																			if (strcmp(command, "FEEDBACK") == 0)
																			{
																				process.setFeedbackCalculation(1);
																				process.runRealtimePipeline();
																				sprintf(command, "OK\n");
																				socks.writeString(command);
																			}
																			else
																				// pipeline function. This is the only function that runs until then end of processing all volumes in the run. Here we set automatic feedback calculations. Needs an open session to work.
																				if (strcmp(command, "NBFEEDBACK") == 0)
																				{
																					process.setPhaseStatus("PIPELINE", 0);
																					process.setPhaseStatus("FEEDBACK", 0);

																					// sending ack
																					sprintf(command, "OK\n");
																					socks.writeString(command);

																					// processing feedback pipeline
																					process.setFeedbackCalculation(1);
																					process.runRealtimePipeline();
																					process.setPhaseStatus("PIPELINE", 1);
																					process.setPhaseStatus("FEEDBACK", 1);
																				}
																				else
																					// sends a terminate command. From here it only exits the thread.
																					if (strcmp(command, "END") == 0)
																					{
																						process.wrapUpRun();
																						sprintf(command, "OK\n");
																						socks.writeString(command);
																						break;
																					}
																					else
																						// exits the engine
																						if (strcmp(command, "EXIT") == 0)
																						{
																							process.wrapUpRun();
																							sprintf(command, "OK\n");
																							socks.writeString(command);
																							exit(0);
																						}
																						else
																							// reads the library filename and the function names in that order : train, test, initialization, finalization, before preprocessing volume and after preprocessing callback. This list will change
																							if (strcmp(command, "PLUGIN") == 0)
																							{
																								char library[500], trainfname[100], testfname[100], initfname[100], finalfname[200], volumefname[200], afterpreprocfname[200];

																								// reading the default config file study_params.txt
																								if (process.isConfigRead() == 0)
																									process.readConfigFile(configFile);

																								// reading library filename and function names. The names must come in order
																								socks.readLine(library, 500);
																								stripReturns(library);

																								socks.readLine(trainfname, 100);
																								stripReturns(trainfname);

																								socks.readLine(testfname, 100);
																								stripReturns(testfname);

																								socks.readLine(initfname, 100);
																								stripReturns(initfname);

																								socks.readLine(finalfname, 100);
																								stripReturns(finalfname);

																								socks.readLine(volumefname, 100);
																								stripReturns(volumefname);

																								socks.readLine(afterpreprocfname, 100);
																								stripReturns(afterpreprocfname);

																								process.loadFunctions(library, trainfname, testfname, initfname, finalfname, volumefname, afterpreprocfname);

																								sprintf(command, "OK\n");
																								socks.writeString(command);
																							}
																							else
																								if (strcmp(command, "SET") == 0)
																								{
																									socks.readLine(tag, 255);
																									socks.readLine(value, 255);
																									stripReturns(tag);
																									stripReturns(value);

																									process.prepRealtimeVars();

																									logObject->writeLog(1, "%s=%s\n", tag, value);
																									process.setVar(tag, value);

																									sprintf(command, "OK\n");
																									socks.writeString(command);
																								}
																								else
																									// this command sends an entire config file to the engine, setting all needed variables
																									if (strcmp(command, "READCONFIG") == 0)
																									{
																										int size = 0;
																										char buffer[5000], number[10];

																										socks.readLine(number, 10);
																										stripReturns(number);
																										size = atoi(number);
																										logObject->writeLog(1, "Size received = %d bytes\n", size);

																										if (socks.readBuffer(buffer, size))
																										{
																											logObject->writeLog(1, "config received= %s\n", buffer);
																											process.readConfigBuffer(buffer, size);
																										}
																										sprintf(command, "OK\n");
																										socks.writeString(command);
																									}
			/*
			// this commands sends an entire config file to engine, and saves it. Thinking in using it.
			else if (strcmp(command, "SAVECONFIG") == 0)
			{
			int size=0;
			char buffer[5000], number[10];

			socks.readLine(number, 10);
			stripReturns(number);
			size = atoi(number);
			logObject->writeLog(1, "Size received = %d bytes\n", size);

			if (socks.readBuffer(buffer, size))
			{
			logObject->writeLog(1, "config received= %s\n", buffer);
			process.saveConfigBuffer(buffer, size, configFile);
			}
			sprintf(command, "OK\n");
			socks.writeString(command);
			}
			*/
		}
	}
	endTime = time(NULL);

	// delaying the thread termination to allow clients to read the last send message
	sleep(1000);
	closesocket(socketFd);
	logObject->writeLog(1, "Finished. Elapsed time = %ld secs.\n", endTime - iniTime);
	return (true);
}

// just a function to start a thread
#ifndef UNIX
bool	WINAPI friendEngineHelper(friendEngine	*App)
{
#else
threadRoutineType friendEngineHelper(void *theApp)
{
	friendEngine *App = (friendEngine *)theApp;
#endif
	int	socketFd;

	// pick up data from passed-in object
	socketFd = App->childSocketFd;
	// release lock on data
	App->lock = 0;
	App->serverChildThread(socketFd);
	return (THREAD_RETURN_FALSE);
}
