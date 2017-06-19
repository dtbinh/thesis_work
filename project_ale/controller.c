// CONTROLLER CODE

#include "controller.h"
#include "startup.h"

#include <unistd.h>
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#include <inttypes.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <errno.h>
#include <semaphore.h>
#include <math.h>
#include <arpa/inet.h>
#include <string.h>

#include <stdlib.h>
#include <time.h>	//usleep

#include "blas.h"
#include "lapack.h"

// PREEMPT_RT
//#include <time.h>
#include <sched.h>
#include <sys/mman.h>

/******************************************************************/
/*******************VARIABLES & PREDECLARATIONS********************/
/******************************************************************/
// Static variables for threads
static float globalSensorData[6]={0,0,0,0,0,0};
static float globalConstraintsData[6]={0,0,0,0,0,0};
static int globalWatchdog=0;
static const int ione = 1;
static const int itwo = 2;
static const int ithree = 3;
static const int iseven = 7;
static const double fone = 1;
static const double ftwo = 2;
static const double fzero = 0;
static const double fmone = -1;
static int quiet = 0;
static struct AltParams {
	double A[4], B[2], Q[4], R, Qf[4], umax, umin, kappa;
	int n, m, T, niters;
	};
static struct AltInputs {
	double X0_all[2*30], U0_all[30], x0[2], xmax[2], xmin[2];
	};

// Controller variables
const static int PosTs = 1e+7; // 1e+7 is 1 sec
const static int AttTs = 1e+7; // 1e+7 is 1 sec
const static int AltTs = 1e+7; // 1e+7 is 1 sec

// Predeclarations
static void *threadUpdateMeasurements(void*);
static void *threadUpdateConstraints(void*);
static void *threadControllerPos(void*);
static void *threadControllerWatchdogPos(void*);
static void *threadControllerAtt(void*);
static void *threadControllerWatchdogAtt(void*);
static void *threadControllerAlt(void*);
static void *threadControllerWatchdogAlt(void*);

static void fmpc(struct AltParams *, struct AltInputs *, double *X, double *U);
static void fmpcsolve(double *A, double *B, double *At, double *Bt, double *eyen, 
        double *eyem, double *Q, double *R, double *Qf, double *zmax, double *zmin, 
        double *x, double *z, int T, int n, int m, int nz, int niters, double kappa);

static void gfgphp(double *Q, double *R, double *Qf, double *zmax, double *zmin, double *z,
        int T, int n, int m, int nz, double *gf, double *gp, double *hp);

static void rdrp(double *A, double *B, double *Q, double *R, double *Qf, double *z, double *nu, 
        double *gf, double *gp, double *b, int T, int n, int m, int nz, 
        double kappa, double *rd, double *rp, double *Ctnu);

static void resdresp(double *rd, double *rp, int T, int n, int nz, double *resd, 
        double *resp, double *res);
        
static void dnudz(double *A, double *B, double *At, double *Bt, double *eyen, 
        double *eyem, double *Q, double *R, double *Qf, double *hp, double *rd, 
        double *rp, int T, int n, int m, int nz, double kappa, double *dnu, double *dz);

void stack_prefault(void);


static pthread_mutex_t mutexSensorData = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t mutexConstraintsData = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t mutexWatchdog = PTHREAD_MUTEX_INITIALIZER;


/******************************************************************/
/*************************START PROCESS****************************/
/******************************************************************/


// Function to start the sensor process threads
void startController(void *arg1, void *arg2){
	// Create pipe array
	pipeArray pipeArrayStruct = {.pipe1 = arg1, .pipe2 = arg2 };
	
	// Create threads
	pthread_t threadUpdateMeas, threadUpdateConstr, threadCtrlPos, threadCtrlAtt, threadCtrlAlt, threadCtrlWDPos, threadCtrlWDAtt, threadCtrlWDAlt;
	int res1, res2, res3, res4, res5, res6, res7, res8;
	
	//res1=pthread_create(&threadUpdateMeas, NULL, &threadUpdateMeasurements, arg1);
	//res2=pthread_create(&threadUpdateConstr, NULL, &threadUpdateConstraints, arg2);
	res3=pthread_create(&threadCtrlPos, NULL, &threadControllerPos, &pipeArrayStruct);
	res4=pthread_create(&threadCtrlWDPos, NULL, &threadControllerWatchdogPos, NULL);
	res5=pthread_create(&threadCtrlAtt, NULL, &threadControllerAtt, &pipeArrayStruct);
	res6=pthread_create(&threadCtrlWDAtt, NULL, &threadControllerWatchdogAtt, NULL);
	res7=pthread_create(&threadCtrlAlt, NULL, &threadControllerAlt, &pipeArrayStruct);
	res8=pthread_create(&threadCtrlWDAlt, NULL, &threadControllerWatchdogAlt, NULL);
	
	// If threads created successful, start them
	//if (!res1) pthread_join( threadUpdateMeas, NULL);
	//if (!res2) pthread_join( threadUpdateConstr, NULL);
	if (!res3) pthread_join( threadCtrlPos, NULL);
	if (!res4) pthread_join( threadCtrlWDPos, NULL);
	if (!res5) pthread_join( threadCtrlAtt, NULL);
	if (!res6) pthread_join( threadCtrlWDAtt, NULL);
	if (!res7) pthread_join( threadCtrlAlt, NULL);
	if (!res8) pthread_join( threadCtrlWDAlt, NULL);
}


/******************************************************************/
/*****************************THREADS******************************/
/******************************************************************/

// Thread - Update constriants from other drones (pipe from communication process).
// Includes any updates on setpoints or MPC settings from computer
void *threadUpdateConstraints(void *arg)
{	
	// Get pipe and define local variables
	structPipe *ptrPipe = arg;
	float constraintsDataBuffer[6] = {0,0,0,0,0,0};
	
	// Loop forever streaming data
	while(1){
		// Read data from communication process
		if(read(ptrPipe->child[0], constraintsDataBuffer, sizeof(constraintsDataBuffer)) == -1) printf("read error in controller from communication\n");
		//else printf("Controller ID: %d, Recieved Communication data: %f\n", (int)getpid(), constraintDataBuffer[0]);
		
		// Put new constraints data in to global data in controller.c such that controller thread can access and use it.
		pthread_mutex_lock(&mutexConstraintsData);
			memcpy(globalConstraintsData, constraintsDataBuffer, sizeof(constraintsDataBuffer));
		pthread_mutex_unlock(&mutexConstraintsData);
	}
	
	return NULL;
}

// Thread - Update local variables with any new sensor measurements (pipe from sensor process)
void *threadUpdateMeasurements(void *arg)
{
	// Get pipe and define local variables
	structPipe *ptrPipe = arg;
	
	float sensorDataBuffer[6] = {0,0,0,0,0,0};
	
	while(1){
		// Read data from sensor process. Data should always be available for controller.
		if(read(ptrPipe->child[0], sensorDataBuffer, sizeof(sensorDataBuffer)) == -1) printf("read error in Controller from Sensor\n");
		//else printf("Controller ID: %d, Recieved Sensor data: %f\n", (int)getpid(), sensorDataBuffer[0])
		
		// Put new sensor data in to global data in controller.c such that controller thread can access and use it.
		pthread_mutex_lock(&mutexSensorData);
			memcpy(globalSensorData, sensorDataBuffer, sizeof(sensorDataBuffer));
		pthread_mutex_unlock(&mutexSensorData);
	}
	
	return NULL;
}

// Thread - Controller algorithm for Position (with pipe to sensor (PWM) and communication process)
void *threadControllerPos(void *arg) {
   
	// Get pipe array and define local variables
	//pipeArray *pipeArrayStruct = arg;
	//structPipe *ptrPipe1 = pipeArrayStruct->pipe1;
	//structPipe *ptrPipe2 = pipeArrayStruct->pipe2;
	
	// Get pipe and define local variables
	struct timespec t;
	struct sched_param param;

	// Declare ourself as a real time task
	param.sched_priority = 39;
	if(sched_setscheduler(getpid(), SCHED_FIFO, &param) == -1){
		perror("sched_setscheduler failed");
	}

	// Lock memory
	if(mlockall(MCL_CURRENT|MCL_FUTURE) == -1){
		perror("mlockall failed");
	}
	
	// Pre-fault our stack
	//stack_prefault();
	
	// Start after 1 second
	clock_gettime(CLOCK_MONOTONIC, &t);
	t.tv_sec++;
	
	// Loop forever at specific sampling rate
	while(1){
		// Wait until next shot
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL);
			
		// Run controller
			
		// Set motor PWM signals by writing to the sensor.c process which applies the changes over I2C.
		//if (write(ptrPipe1->parent[1], value, sizeof(value)) != sizeof(value)) printf("write error in controller to sensor\n");
		//else printf("Controller ID: %d, Sent: %f to Communication\n", (int)getpid(), controllerDataBuffer[0]);
	
		// Set update of constraints and controller results by writing to the communication.c process which applies the changes over UDP.
		//if (write(ptrPipe2->parent[1], controllerDataBuffer, sizeof(controllerDataBuffer)) != sizeof(controllerDataBuffer)) printf("write error in controller to communication\n");
		//else printf("Controller ID: %d, Sent: %f to Communication\n", (int)getpid(), controllerDataBuffer[0]);

		// Update watchdog
		pthread_mutex_lock(&mutexWatchdog);
			globalWatchdog++;
		pthread_mutex_unlock(&mutexWatchdog);
		
		// Calculate next shot
		t.tv_nsec += PosTs;	//	nanosec sampling time
		while (t.tv_nsec >= NSEC_PER_SEC) {
			t.tv_nsec -= NSEC_PER_SEC;
			t.tv_sec++;
		}
	}
	
	return NULL;
}

// Thread - Watchdog for Position controller to flag if sampling time is not satisfied.
void *threadControllerWatchdogPos(void *arg) {	
	// Get pipe and define local variables
	struct timespec t;
	struct sched_param param;
	int watchdog_current, watchdog_prev=0;

	// Declare ourself as a real time task
	param.sched_priority = 40;
	if(sched_setscheduler(getpid(), SCHED_FIFO, &param) == -1){
		perror("sched_setscheduler failed");
	}
	
	// Lock memory
	if(mlockall(MCL_CURRENT|MCL_FUTURE) == -1){
		perror("mlockall failed");
	}
	
	// Pre-fault our stack
	//stack_prefault();
	
	// Start after 1 second
	clock_gettime(CLOCK_MONOTONIC, &t);
	t.tv_sec++;
	
	// Run controller algorithm
	while(1){
		// Wait until next shot
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL);
		
		// Get watchdog status
		pthread_mutex_lock(&mutexWatchdog);
			watchdog_current=globalWatchdog; // current
			globalWatchdog=watchdog_prev+1; // update to new
		pthread_mutex_unlock(&mutexWatchdog);
		
		// Check if deadline was met
		if (watchdog_current==watchdog_prev){
			printf("MPC did NOT meet deadline2\n");
		}
		
		// Update previous watchdog to current
		watchdog_prev++;
		
		// Calculate next shot
		t.tv_sec += PosTs;
		while (t.tv_nsec >= NSEC_PER_SEC) {
			t.tv_nsec -= NSEC_PER_SEC;
			t.tv_sec++;
		}
	}
	
	return NULL;
}

// Thread - Controller algorithm for Attitude (with pipe to sensor (PWM) and communication process)
void *threadControllerAtt(void *arg) {
	double x_all[12] = { 1,2,3,4,5,6,7,8,9,10,11,12 };	//silly measurements for test
    
	// Get pipe array and define local variables
	//pipeArray *pipeArrayStruct = arg;
	//structPipe *ptrPipe1 = pipeArrayStruct->pipe1;
	//structPipe *ptrPipe2 = pipeArrayStruct->pipe2;
	
	// Get pipe and define local variables
	struct timespec t;
	struct sched_param param;

	// Declare ourself as a real time task
	param.sched_priority = 39;
	if(sched_setscheduler(getpid(), SCHED_FIFO, &param) == -1){
		perror("sched_setscheduler failed");
	}

	// Lock memory
	if(mlockall(MCL_CURRENT|MCL_FUTURE) == -1){
		perror("mlockall failed");
	}
	
	// Pre-fault our stack
	//stack_prefault();
	
	// Start after 1 second
	clock_gettime(CLOCK_MONOTONIC, &t);
	t.tv_sec++;
	
	// Loop forever at specific sampling rate
	while(1){
		// Wait until next shot
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL);
			
		// Run controller
		
			
		// Set motor PWM signals by writing to the sensor.c process which applies the changes over I2C.
		//if (write(ptrPipe1->parent[1], value, sizeof(value)) != sizeof(value)) printf("write error in controller to sensor\n");
		//else printf("Controller ID: %d, Sent: %f to Communication\n", (int)getpid(), controllerDataBuffer[0]);
	
		// Set update of constraints and controller results by writing to the communication.c process which applies the changes over UDP.
		//if (write(ptrPipe2->parent[1], controllerDataBuffer, sizeof(controllerDataBuffer)) != sizeof(controllerDataBuffer)) printf("write error in controller to communication\n");
		//else printf("Controller ID: %d, Sent: %f to Communication\n", (int)getpid(), controllerDataBuffer[0]);

		// Update watchdog
		pthread_mutex_lock(&mutexWatchdog);
			globalWatchdog++;
		pthread_mutex_unlock(&mutexWatchdog);
		
		// Calculate next shot
		t.tv_nsec += AttTs;	//	nanosec sampling time
		while (t.tv_nsec >= NSEC_PER_SEC) {
			t.tv_nsec -= NSEC_PER_SEC;
			t.tv_sec++;
		}
	}
	
	return NULL;
}

// Thread - Watchdog for Attitude controller to flag if sampling time is not satisfied.
void *threadControllerWatchdogAtt(void *arg) {	
	// Get pipe and define local variables
	struct timespec t;
	struct sched_param param;
	int watchdog_current, watchdog_prev=0;

	// Declare ourself as a real time task
	param.sched_priority = 40;
	if(sched_setscheduler(getpid(), SCHED_FIFO, &param) == -1){
		perror("sched_setscheduler failed");
	}
	
	// Lock memory
	if(mlockall(MCL_CURRENT|MCL_FUTURE) == -1){
		perror("mlockall failed");
	}
	
	// Pre-fault our stack
	//stack_prefault();
	
	// Start after 1 second
	clock_gettime(CLOCK_MONOTONIC, &t);
	t.tv_sec++;
	
	// Run controller algorithm
	while(1){
		// Wait until next shot
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL);
		
		// Get watchdog status
		pthread_mutex_lock(&mutexWatchdog);
			watchdog_current=globalWatchdog; // current
			globalWatchdog=watchdog_prev+1; // update to new
		pthread_mutex_unlock(&mutexWatchdog);
		
		// Check if deadline was met
		if (watchdog_current==watchdog_prev){
			printf("MPC did NOT meet deadline2\n");
		}
		
		// Update previous watchdog to current
		watchdog_prev++;
		
		// Calculate next shot
		t.tv_sec += AttTs;
		while (t.tv_nsec >= NSEC_PER_SEC) {
			t.tv_nsec -= NSEC_PER_SEC;
			t.tv_sec++;
		}
	}
	
	return NULL;
}

// Thread - Controller algorithm for Altitude (with pipe to sensor (PWM) and communication process)
void *threadControllerAlt(void *arg) {
		
	//double x_all[12] = { 1,2,3,4,5,6,7,8,9,10,11,12 };	//silly measurements for test
    const struct AltParams altParams = { 
		.A = { 1, PosTs*1e-9, 0, 1 },
		.B = { 0, PosTs*1e-9 },
		.Q = { 100, 1, 1, 100 },
		.Qf = { 100, 1, 1, 100 },
		.R = 10,
		.umax = 100,
		.umin = 1,
		.n = 2, .m = 1, .T = 2, .niters = 5, .kappa = 1e-3
	};
	struct AltInputs altInputs = { 
		.X0_all = { 1, 2, 1.1, 2.2 },
		.U0_all = { 10, 10 }, 
		.x0 = { 0.9, 1.9 },
		.xmax = { 50, 50 },
		.xmin = { -50, -50 },
	};
 
    
	// Get pipe array and define local variables
	//pipeArray *pipeArrayStruct = arg;
	//structPipe *ptrPipe1 = pipeArrayStruct->pipe1;
	//structPipe *ptrPipe2 = pipeArrayStruct->pipe2;
	
	// Get pipe and define local variables
	struct timespec t;
	struct sched_param param;

	// Declare ourself as a real time task
	param.sched_priority = 39;
	if(sched_setscheduler(getpid(), SCHED_FIFO, &param) == -1){
		perror("sched_setscheduler failed");
	}

	// Lock memory
	if(mlockall(MCL_CURRENT|MCL_FUTURE) == -1){
		perror("mlockall failed");
	}
	
	// Pre-fault our stack
	//stack_prefault();
	
	// Start after 1 second
	clock_gettime(CLOCK_MONOTONIC, &t);
	t.tv_sec++;
	
	// Loop forever at specific sampling rate
	while(1){
		// Wait until next shot
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL);
			
		// Run controller
		fmpc(&altParams, &altInputs, NULL, NULL);
		//printf("%i", posTs);
			
		// Set motor PWM signals by writing to the sensor.c process which applies the changes over I2C.
		//if (write(ptrPipe1->parent[1], value, sizeof(value)) != sizeof(value)) printf("write error in controller to sensor\n");
		//else printf("Controller ID: %d, Sent: %f to Communication\n", (int)getpid(), controllerDataBuffer[0]);
	
		// Set update of constraints and controller results by writing to the communication.c process which applies the changes over UDP.
		//if (write(ptrPipe2->parent[1], controllerDataBuffer, sizeof(controllerDataBuffer)) != sizeof(controllerDataBuffer)) printf("write error in controller to communication\n");
		//else printf("Controller ID: %d, Sent: %f to Communication\n", (int)getpid(), controllerDataBuffer[0]);

		// Update watchdog
		pthread_mutex_lock(&mutexWatchdog);
			globalWatchdog++;
		pthread_mutex_unlock(&mutexWatchdog);
		
		// Calculate next shot
		t.tv_nsec += AltTs;	//	nanosec sampling time
		while (t.tv_nsec >= NSEC_PER_SEC) {
			t.tv_nsec -= NSEC_PER_SEC;
			t.tv_sec++;
		}
	}
	
	return NULL;
}

// Thread - Watchdog for Altitude controller to flag if sampling time is not satisfied.
void *threadControllerWatchdogAlt(void *arg) {	
	// Get pipe and define local variables
	struct timespec t;
	struct sched_param param;
	int watchdog_current, watchdog_prev=0;

	// Declare ourself as a real time task
	param.sched_priority = 40;
	if(sched_setscheduler(getpid(), SCHED_FIFO, &param) == -1){
		perror("sched_setscheduler failed");
	}
	
	// Lock memory
	if(mlockall(MCL_CURRENT|MCL_FUTURE) == -1){
		perror("mlockall failed");
	}
	
	// Pre-fault our stack
	//stack_prefault();
	
	// Start after 1 second
	clock_gettime(CLOCK_MONOTONIC, &t);
	t.tv_sec++;
	
	// Run controller algorithm
	while(1){
		// Wait until next shot
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL);
		
		// Get watchdog status
		pthread_mutex_lock(&mutexWatchdog);
			watchdog_current=globalWatchdog; // current
			globalWatchdog=watchdog_prev+1; // update to new
		pthread_mutex_unlock(&mutexWatchdog);
		
		// Check if deadline was met
		if (watchdog_current==watchdog_prev){
			printf("MPC did NOT meet deadline2\n");
		}
		
		// Update previous watchdog to current
		watchdog_prev++;
		
		// Calculate next shot
		t.tv_sec += AltTs;
		while (t.tv_nsec >= NSEC_PER_SEC) {
			t.tv_nsec -= NSEC_PER_SEC;
			t.tv_sec++;
		}
	}
	
	return NULL;
}


/******************************************************************/
/****************************FUNCTIONS*****************************/
/******************************************************************/

void stack_prefault(void){
	unsigned char dummy[MAX_SAFE_STACK];
	
	memset(dummy, 0, MAX_SAFE_STACK);
	return;
}

/* function to interact with fast MPC */
static void fmpc( struct AltParams *altParams, struct AltInputs *altInputs, double *X, double *U){

	/* problem setup */
    int i, j, m, n, nz, T, niters, k;
    double kappa;
    double *dptr, *dptr1, *dptr2;
    const double *Cdptr;
    double *A, *B, *At, *Bt, *Q, *R, *Qf, *xmax, *xmin, *umax, *umin, *x;
    double *zmax, *zmin, *zmaxp, *zminp, *X_all, *U_all, *z, *eyen, *eyem, *x0;
    double *X0_all, *U0_all;
    //int agent_mode = 0;
    double *telapsed;
    clock_t t1, t2;
    
	nz = altParams->T * (altParams->n + altParams->m);
	
	printf("%i", nz);
    
    ///* Allocate memory for inputs */
    //X0_all = malloc(sizeof(double)*params->n*params->T);
    //U0_all = malloc(sizeof(double)*params->m*params->T);
    //x0 = malloc(sizeof(double)*params->n*1);
    //xmax = malloc(sizeof(double)*params->n);
    //xmin = malloc(sizeof(double)*params->n);

	/////* Reading from the input */
    ////Cdptr = uPtrs[0];
    ////for (i = 0; i < T; i++) {   //col-major X0_temp
        ////for (j = 0; j < n; j++) {
            ////X0_all[i*n+j] = *Cdptr++;
//////             *y++ = X0_temp[i*n+j];
//////             Cdptr++;
        ////}
    ////}
    ////for (i = 0; i < T; i++) {   //col-major U0_temp
        ////for (j = 0; j < m; j++) {
            ////U0_all[i*m+j] = *Cdptr++;
//////             *y++ = U0_temp[i*m+j];
//////             printf("%f\n", U0_temp[i*m+j]);
//////             Cdptr++;
        ////}
        ////for (j = 0; j < n-m; j++) { //forward the zero-padded part of the coloumn
//////             *y++ = *Cdptr;
//////             printf("%f\n", *Cdptr);
//////             if ( ~isnan(*Cdptr) ) printf("ERROR in input reading!!!\n");
//////             Cdptr++; //it must be NAN
            ////Cdptr++;
        ////}
    ////}
    ////for (j = 0; j < n; j++) {   //col-major x0
        ////x0[j] = *Cdptr++;
//////         *y++ = x0[i*m+j];
//////         printf("%f\n", x0[i*m+j]);
//////         Cdptr++;
    ////}
	/////* Reading from exact linearizzation
    ////REMEMBER TO FREE THE MEMOMRY!!! */
    ////for (i = 0; i < (n)*(n); i++) {   //col-major A
        ////A[i] = *Cdptr++;
    ////}
////// 
    ////for (i = 0; i < (n)*m; i++) {   //col-major B
        ////B[i] = *Cdptr++;
    ////}
//////     
    ////agent_mode = *Cdptr++;
//////     printf("agent_mode %i \n", agent_mode);
    ////for (i = 0; i < n; i++) {
        ////xmin[i] = *Cdptr++;
//////         printf("xmin %i is %f\n", i, xmin[i]);
    ////}
    ////for (i = 0; i < n; i++) {
        ////xmax[i] = *Cdptr++;
//////         printf("xmax %i is %f\n", i, xmax[i]);
    ////}
//////     printf("---\n"); 
//////  

    /* outputs */
    X_all = malloc(sizeof(double)*altParams->n*altParams->T);
    U_all = malloc(sizeof(double)*altParams->m*altParams->T);
    telapsed = malloc(sizeof(double));
    At = malloc(sizeof(double)*altParams->n*altParams->n);
    Bt = malloc(sizeof(double)*altParams->n*altParams->m);
    eyen = malloc(sizeof(double)*altParams->n*altParams->n);
    eyem = malloc(sizeof(double)*altParams->m*altParams->m);
    z = malloc(sizeof(double)*nz);
    x = malloc(sizeof(double)*altParams->n);
    zmax = malloc(sizeof(double)*nz);
    zmin = malloc(sizeof(double)*nz);
    zmaxp = malloc(sizeof(double)*nz);
    zminp = malloc(sizeof(double)*nz);
//     
    /* eyen, eyem */
    dptr = eyen;
    for (i = 0; i < altParams->n*altParams->n; i++)
    {
        *dptr = 0;
        dptr++;
    }
    dptr = dptr-altParams->n*altParams->n;
    for (i = 0; i < altParams->n; i++)
    {
        *dptr = 1;
        dptr = dptr+altParams->n+1;
    }
// 
    dptr = eyem;
    for (i = 0; i < altParams->m*altParams->m; i++)
    {
        *dptr = 0;
        dptr++;
    }
    dptr = dptr-altParams->m*altParams->m;
    for (i = 0; i < altParams->m; i++)
    {
        *(dptr+i*altParams->m+i) = 1;
    }
    dptr = x; dptr1 = x0;
    for (i = 0; i < altParams->n; i++)
    {
        *dptr = *dptr1;
        dptr++; dptr1++;
    }
    dptr = z;
    for (i = 0; i < altParams->T; i++)
    {
        for (j = 0; j < altParams->m; j++)
        {
            *dptr = *(altInputs->U0_all+i*altParams->m+j);
            dptr++;
        }
        for (j = 0; j < altParams->n; j++)
        {
            *dptr = *(altInputs->X0_all+i*altParams->n+j);
            dptr++; 
        }
    }  
    /* At, Bt */
    F77_CALL(dgemm)("t","n",&altParams->n,&altParams->n,&altParams->n,&fone,altParams->A,&altParams->n,eyen,&altParams->n,&fzero,At,&altParams->n);
    F77_CALL(dgemm)("n","t",&altParams->m,&altParams->n,&altParams->m,&fone,eyem,&altParams->m,altParams->B,&altParams->n,&fzero,Bt,&altParams->m);
// 
    /* zmax, zmin */
    dptr1 = zmax;
    dptr2 = zmin;
    for (i = 0; i < altParams->T; i++)
    {
        for (j = 0; j < altParams->m; j++)
        {
            *dptr1 = (altParams->umax+j);	// exception because altParams-> is one value and not pointer to an array
            *dptr2 = (altParams->umin+j);
            dptr1++; dptr2++;
        }
        for (j = 0; j < altParams->n; j++)
        {
            *dptr1 = *(altInputs->xmax+j);
            *dptr2 = *(altInputs->xmin+j);
            dptr1++; dptr2++;
        }
    }  
// 
    ///* zmaxp, zminp */
    //for (i = 0; i < nz; i++) zminp[i] = zmin[i] + 0.01*(zmax[i]-zmin[i]);
    //for (i = 0; i < nz; i++) zmaxp[i] = zmax[i] - 0.01*(zmax[i]-zmin[i]);
//// 
    ///* project z */
    //for (i = 0; i < nz; i++) z[i] = z[i] > zmaxp[i] ? zmaxp[i] : z[i];
    //for (i = 0; i < nz; i++) z[i] = z[i] < zminp[i] ? zminp[i] : z[i];
     
     //printf("A\n");
     //printmat(A, params->n, params->n);
     //printf("B\n");
     //printmat(B, n, m);
     //printf("At\n");
     //printmat(At, n, n);
     //printf("Bt\n");
     //printmat(Bt, m, n);
     //printf("n = %i | m = %i | T = %i | niters = %i | kappa = %f\n", params->n, params->m, params->T, params->niters, params->kappa);
     //printf("eyen\n");
     //printmat(eyen, n, n);
     //printf("eyem\n");
     //printmat(eyem, m, m);
     //printf("Q\n");
     //printmat(Q, n, n);
     //printf("R\n");
     //printmat(R, m, m);
     //printf("Qf\n");
     //printmat(Qf, n, n);
     //printf("zmax\n");
     //printmat(zmax, n+m, T);
     //printf("zmin\n");
     //printmat(zmin, n+m, T);
     //printf("x\n");
     //printmat(x, n, 1);
     //printf("z\n");
     //printmat(z, n+m, 1);
 
    //t1 = clock();
    //fmpcsolve(A,B,At,Bt,eyen,eyem,Q,R,Qf,zmax,zmin,x,z,T,n,m,nz,niters,kappa);
    //t2 = clock();
    //*telapsed = (double)(t2-t1)/(CLOCKS_PER_SEC);
////     
    //dptr = z;
    //for (i = 0; i < T; i++)
    //{
        //for (j = 0; j < m; j++)
        //{
            //*(U_all+i*m+j) = *dptr;
            //*y++ = *dptr;//output
////             *y++ = 0;//output
            //dptr++;
        //}
        //for (j = 0; j < n; j++)
        //{
            //*(X_all+i*n+j) = *dptr;
            //*y++ = *dptr;//output
////             *y++ = 0;//output
            //dptr++;
        //}
    //}
    //*y++ = *telapsed;//output
//// 
////     for (i = 0; i < yWidth; i++) {
////             *y++ = i+1;
////     }
////     
////     free(At); free(Bt); free(eyen); free(eyem);
////     free(z); free(x); free(zmax); free(zmin);
////     free(A); free(B);
////     free(xmin); free(xmax);
    
    //free(X0_all); free(U0_all); free(x0); free(xmax); free(xmin);
    //free(A); free(B); 
    free(X_all); free(U_all); free(telapsed);
    free(At); free(Bt); free(eyen); free(eyem);
    free(z); free(x); free(zmax); free(zmin); free(zmaxp); free(zminp);
    return;
}

void fmpcsolve(double *A, double *B, double *At, double *Bt, double *eyen,
         double *eyem, double *Q, double *R, double *Qf, double *zmax, double *zmin, 
         double *x, double *z0, int T, int n, int m, int nz, int niters, double kappa) {
    int maxiter = niters;
    int iiter, i, cont;
    double alpha = 0.01;
    double beta = 0.9;
    double tol = 0.1;
    double s;
    double resd, resp, res, newresd, newresp, newres;
    double *b, *z, *nu, *Ctnu;
    double *dnu, *dz;
    double *gf, *gp, *hp, *newgf, *newgp, *newhp;
    double *rd, *rp, *newrd, *newrp;
    double *dptr, *dptr1, *dptr2, *dptr3;
    double *newnu, *newz, *newCtnu;

    /* memory allocation */
    b = malloc(sizeof(double)*T*n);
    dnu = malloc(sizeof(double)*T*n);
    dz = malloc(sizeof(double)*nz);
    nu = malloc(sizeof(double)*T*n);
    Ctnu = malloc(sizeof(double)*nz);
    z = malloc(sizeof(double)*nz);
    gp = malloc(sizeof(double)*nz);
    hp = malloc(sizeof(double)*nz);
    gf = malloc(sizeof(double)*nz);
    rp = malloc(sizeof(double)*T*n);
    rd = malloc(sizeof(double)*nz);
    newnu = malloc(sizeof(double)*T*n);
    newz = malloc(sizeof(double)*nz);
    newCtnu = malloc(sizeof(double)*nz);
    newgf = malloc(sizeof(double)*nz);
    newhp = malloc(sizeof(double)*nz);
    newgp = malloc(sizeof(double)*nz);
    newrp = malloc(sizeof(double)*T*n);
    newrd = malloc(sizeof(double)*nz);

    for (i = 0; i < n*T; i++) nu[i] = 0;
    for (i = 0; i < n*T; i++) b[i] = 0;

    F77_CALL(dgemv)("n",&n,&n,&fone,A,&n,x,&ione,&fzero,b,&ione);
    dptr = z; dptr1 = z0;
    for (i = 0; i < nz; i++) 
    {
        *dptr = *dptr1;
        dptr++; dptr1++;
    }
    if (quiet == 0)
    {   
        //printf("\n iteration \t step \t\t rd \t\t\t rp\n");
    }
    for (iiter = 0; iiter < maxiter; iiter++)
    {
        gfgphp(Q,R,Qf,zmax,zmin,z,T,n,m,nz,gf,gp,hp);
        rdrp(A,B,Q,R,Qf,z,nu,gf,gp,b,T,n,m,nz,kappa,rd,rp,Ctnu);
        resdresp(rd,rp,T,n,nz,&resd,&resp,&res);

        if (res < tol) break;

        dnudz(A,B,At,Bt,eyen,eyem,Q,R,Qf,hp,rd,rp,T,n,m,nz,kappa,dnu,dz); 

        s = 1; 
        /* feasibility search */
        while (1)
        {
            cont = 0;
            dptr = z; dptr1 = dz; dptr2 = zmax; dptr3 = zmin;
            for (i = 0; i < nz; i++)
            {
                if (*dptr+s*(*dptr1) >= *dptr2) cont = 1;
                if (*dptr+s*(*dptr1) <= *dptr3) cont = 1;
                dptr++; dptr1++; dptr2++; dptr3++;
            }
            if (cont == 1)
            {
                s = beta*s;
                continue;
            }
            else
                break;
        }

        dptr = newnu; dptr1 = nu; dptr2 = dnu;
        for (i = 0; i < T*n; i++)
        {
            *dptr = *dptr1+s*(*dptr2);
            dptr++; dptr1++; dptr2++;
        }
        dptr = newz; dptr1 = z; dptr2 = dz;
        for (i = 0; i < nz; i++)
        {
            *dptr = *dptr1+s*(*dptr2);
            dptr++; dptr1++; dptr2++;
        }

        /* insert backtracking line search */
        while (1)
        {
            gfgphp(Q,R,Qf,zmax,zmin,newz,T,n,m,nz,newgf,newgp,newhp);
            rdrp(A,B,Q,R,Qf,newz,newnu,newgf,newgp,b,T,n,m,nz,kappa,newrd,newrp,newCtnu);
            resdresp(newrd,newrp,T,n,nz,&newresd,&newresp,&newres);
            if (newres <= (1-alpha*s)*res) break;
            s = beta*s;
            dptr = newnu; dptr1 = nu; dptr2 = dnu;
            for (i = 0; i < T*n; i++)
            {
                *dptr = *dptr1+s*(*dptr2);
                dptr++; dptr1++; dptr2++;
            }
            dptr = newz; dptr1 = z; dptr2 = dz;
            for (i = 0; i < nz; i++)
            {
                *dptr = *dptr1+s*(*dptr2);
                dptr++; dptr1++; dptr2++;
            }
        }
        
        dptr = nu; dptr1 = newnu; 
        for (i = 0; i < T*n; i++)
        {
            *dptr = *dptr1;
            dptr++; dptr1++;
        }
        dptr = z; dptr1 = newz;
        for (i = 0; i < nz; i++)
        {
            *dptr = *dptr1;
            dptr++; dptr1++;
        }
        if (quiet == 0)
        {
            //printf("    %d \t\t %5.4f \t %0.5e \t\t %0.5e\n",iiter,s,newresd,newresp);
        }
    }
    dptr = z0; dptr1 = z;
    for (i = 0; i < nz; i++)
    {
        *dptr = *dptr1;
        dptr++; dptr1++;
    }

    free(b); free(dnu); free(dz); free(nu); free(Ctnu);
    free(z); free(gp); free(hp); free(gf); free(rp); free(rd);
    free(newnu); free(newz); free(newCtnu); free(newgf); free(newhp);
    free(newgp); free(newrp); free(newrd);
    return;
}

/* computes the search directions dz and dnu */
void dnudz(double *A, double *B, double *At, double *Bt, double *eyen,
        double *eyem, double *Q, double *R, double *Qf, double *hp, double *rd, 
        double *rp, int T, int n, int m, int nz, double kappa, double *dnu, double *dz)
{
    int i,j,info,nT;
    double *dptr, *dptr1, *dptr2, *dptr3, *temp, *tempmatn, *tempmatm;
    double *PhiQ, *PhiR, *Yd, *Yud, *Ld, *Lld, *Ctdnu, *gam, *v, *be, *rdmCtdnu;
    double *PhiinvQAt, *PhiinvRBt, *PhiinvQeye, *PhiinvReye, *CPhiinvrd;
    nT = n*T;

    /* allocate memory */
    PhiQ = malloc(sizeof(double)*n*n*T);
    PhiR = malloc(sizeof(double)*m*m*T);
    PhiinvQAt = malloc(sizeof(double)*n*n*T);
    PhiinvRBt = malloc(sizeof(double)*m*n*T);
    PhiinvQeye = malloc(sizeof(double)*n*n*T);
    PhiinvReye = malloc(sizeof(double)*m*m*T);
    CPhiinvrd = malloc(sizeof(double)*n*T);
    Yd = malloc(sizeof(double)*n*n*T);
    Yud = malloc(sizeof(double)*n*n*(T-1));
    Ld = malloc(sizeof(double)*n*n*T);
    Lld = malloc(sizeof(double)*n*n*(T-1));
    gam = malloc(sizeof(double)*n*T);
    v = malloc(sizeof(double)*n*T);
    be = malloc(sizeof(double)*n*T);
    temp = malloc(sizeof(double)*n);
    tempmatn = malloc(sizeof(double)*n*n);
    tempmatm = malloc(sizeof(double)*m*m);
    Ctdnu = malloc(sizeof(double)*nz);
    rdmCtdnu = malloc(sizeof(double)*nz);

    /* form PhiQ and PhiR */
    for (i = 0; i < T-1; i++)
    {
        dptr = PhiQ+n*n*i; dptr1 = Q;
        for (j = 0; j < n*n; j++)
        {
            *dptr = 2*(*dptr1);
            dptr++; dptr1++;
        }
        dptr = PhiQ+n*n*i; dptr1 = hp+m*(i+1)+n*i;
        for (j = 0; j < n; j++)
        {
            *dptr = *dptr+kappa*(*dptr1);
            dptr = dptr+n+1; dptr1++;
        }
        dptr = PhiR+m*m*i; dptr1 = R;
        for (j = 0; j < m*m; j++)
        {
            *dptr = 2*(*dptr1);
            dptr++; dptr1++;
        }
        dptr = PhiR+m*m*i; dptr1 = hp+i*(n+m);
        for (j = 0; j < m; j++)
        {
            *dptr = *dptr+kappa*(*dptr1);
            dptr = dptr+m+1; dptr1++;
        }
    }
    
    dptr = PhiR+m*m*(T-1); dptr1 = R;
    for (j = 0; j < m*m; j++)
    {
        *dptr = 2*(*dptr1);
        dptr++; dptr1++;
    }
    dptr = PhiR+m*m*(T-1); dptr1 = hp+(T-1)*(n+m);
    for (j = 0; j < m; j++)
    {
        *dptr = *dptr+kappa*(*dptr1);
        dptr = dptr+m+1; dptr1++;
    }
    dptr = PhiQ+n*n*(T-1); dptr1 = Qf;
    for (j = 0; j < n*n; j++)
    {
        *dptr = 2*(*dptr1);
        dptr++; dptr1++;
    }
    dptr = PhiQ+n*n*(T-1); dptr1 = hp+m*T+n*(T-1);
    for (j = 0; j < n; j++)
    {
        *dptr = *dptr+kappa*(*dptr1);
        dptr = dptr+n+1; dptr1++;
    }

    /* compute PhiinvQAt, PhiinvRBt, PhiinvQeye, PhiinvReye */
    for (i = 0; i < T; i++)
    {
        dptr = PhiinvQAt+n*n*i; dptr1 = At;
        for (j = 0; j < n*n; j++)
        {
            *dptr = *dptr1;
            dptr++; dptr1++;
        }
        dptr = dptr-n*n; dptr1 = PhiQ+n*n*i;
        F77_CALL(dposv)("l",&n,&n,dptr1,&n,dptr,&n,&info);
        dptr = PhiinvQeye+n*n*i; dptr1 = eyen;
        for (j = 0; j < n*n; j++)
        {
            *dptr = *dptr1;
            dptr++; dptr1++;
        }
        dptr = dptr-n*n; dptr1 = PhiQ+n*n*i;
        F77_CALL(dtrtrs)("l","n","n",&n,&n,dptr1,&n,dptr,&n,&info);
        F77_CALL(dtrtrs)("l","t","n",&n,&n,dptr1,&n,dptr,&n,&info);
    }
    for (i = 0; i < T; i++)
    {
        dptr = PhiinvRBt+m*n*i; dptr1 = Bt;
        for (j = 0; j < n*m; j++)
        {
            *dptr = *dptr1;
            dptr++; dptr1++;
        }
        dptr = dptr-m*n; dptr1 = PhiR+m*m*i;
        F77_CALL(dposv)("l",&m,&n,dptr1,&m,dptr,&m,&info);
        dptr = PhiinvReye+m*m*i; dptr1 = eyem;
        for (j = 0; j < m*m; j++)
        {
            *dptr = *dptr1;
            dptr++; dptr1++;
        }
        dptr = dptr-m*m; dptr1 = PhiR+m*m*i;
        F77_CALL(dtrtrs)("l","n","n",&m,&m,dptr1,&m,dptr,&m,&info);
        F77_CALL(dtrtrs)("l","t","n",&m,&m,dptr1,&m,dptr,&m,&info);
    }
    
    /* form Yd and Yud */
    dptr = Yud; dptr1 = PhiinvQAt; 
    for (i = 0; i < n*n*(T-1); i++)
    {
        *dptr = -(*dptr1);
        dptr++; dptr1++;
    }
    dptr2 = Yd; dptr3 = PhiinvQeye;
    for (i = 0; i < n*n; i++)
    {
        *dptr2 = *dptr3;
        dptr2++; dptr3++;
    }
    dptr2 = dptr2-n*n;
    F77_CALL(dgemm)("n","n",&n,&n,&m,&fone,B,&n,PhiinvRBt,&m,&fone,dptr2,&n);
    for (i = 1; i < T; i++)
    {
        dptr = Yd+n*n*i; dptr1 = PhiinvQeye+n*n*i;
        for (j = 0; j < n*n; j++)
        {
            *dptr = *dptr1;
            dptr++; dptr1++;
        }
        dptr1 = PhiinvRBt+m*n*i; dptr = dptr-n*n;
        F77_CALL(dgemm)("n","n",&n,&n,&m,&fone,B,&n,dptr1,&m,&fone,dptr,&n); 
        dptr1 = PhiinvQAt+n*n*(i-1);
        F77_CALL(dgemm)("n","n",&n,&n,&n,&fone,A,&n,dptr1,&n,&fone,dptr,&n);
    }

    /* compute Lii */
    dptr = Ld; dptr1 = Yd; 
    for (i = 0; i < n*n; i++)
    {
        *dptr = *dptr1;
        dptr++; dptr1++; 
    }
    dptr = dptr-n*n; 
    F77_CALL(dposv)("l",&n,&ione,dptr,&n,temp,&n,&info);
    for (i = 1; i < T; i++)
    {
        dptr = Ld+n*n*(i-1); dptr1 = Yud+n*n*(i-1); dptr2 = Lld+n*n*(i-1);
        for (j = 0; j < n*n; j++)
        {
            *dptr2 = *dptr1;
            dptr2++; dptr1++;
        }
        dptr2 = dptr2-n*n;
        F77_CALL(dtrtrs)("l","n","n",&n,&n,dptr,&n,dptr2,&n,&info);
        dptr1 = tempmatn;
        for (j = 0; j < n*n; j++)
        {
            *dptr1 = *dptr2;
            dptr1++; dptr2++;
        }
        dptr1 = dptr1-n*n; dptr2 = dptr2-n*n;
        F77_CALL(dgemm)("t","n",&n,&n,&n,&fone,dptr1,&n,eyen,&n,&fzero,dptr2,&n);
        dptr = Ld+n*n*i; dptr1 = Yd+n*n*i;
        for (j = 0; j < n*n; j++)
        {
            *dptr = *dptr1;
            dptr++; dptr1++;
        }
        dptr = dptr-n*n;
        F77_CALL(dgemm)("n","t",&n,&n,&n,&fmone,dptr2,&n,dptr2,&n,&fone,dptr,&n);
        F77_CALL(dposv)("l",&n,&ione,dptr,&n,temp,&n,&info);
    }

    /* compute CPhiinvrd */
    dptr = CPhiinvrd; dptr1 = rd+m;
    for (i = 0; i < n; i++)
    {
        *dptr = *dptr1;
        dptr++; dptr1++;
    }
    dptr = dptr-n;
    F77_CALL(dtrsv)("l","n","n",&n,PhiQ,&n,dptr,&ione);
    F77_CALL(dtrsv)("l","t","n",&n,PhiQ,&n,dptr,&ione);
    dptr2 = temp; dptr1 = rd;
    for (i = 0; i < m; i++)
    {
        *dptr2 = *dptr1;
        dptr2++; dptr1++;
    }
    dptr2 = dptr2-m;
    F77_CALL(dtrsv)("l","n","n",&m,PhiR,&m,dptr2,&ione);
    F77_CALL(dtrsv)("l","t","n",&m,PhiR,&m,dptr2,&ione);
    F77_CALL(dgemv)("n",&n,&m,&fmone,B,&n,temp,&ione,&fone,dptr,&ione);
    
    for (i = 1; i < T; i++)
    {
        dptr = CPhiinvrd+n*i; dptr1 = rd+m+i*(n+m);
        for (j = 0; j < n; j++)
        {
            *dptr = *dptr1;
            dptr++; dptr1++;
        }
        dptr = dptr-n; dptr3 = PhiQ+n*n*i;
        F77_CALL(dtrsv)("l","n","n",&n,dptr3,&n,dptr,&ione);
        F77_CALL(dtrsv)("l","t","n",&n,dptr3,&n,dptr,&ione);
        dptr2 = temp; dptr1 = rd+i*(m+n);
        for (j = 0; j < m; j++)
        {
            *dptr2 = *dptr1;
            dptr2++; dptr1++;
        }
        dptr3 = PhiR+m*m*i; dptr2 = dptr2-m;
        F77_CALL(dtrsv)("l","n","n",&m,dptr3,&m,dptr2,&ione);
        F77_CALL(dtrsv)("l","t","n",&m,dptr3,&m,dptr2,&ione);
        F77_CALL(dgemv)("n",&n,&m,&fmone,B,&n,temp,&ione,&fone,dptr,&ione);
        dptr2 = temp; dptr1 = rd+(i-1)*(n+m)+m;
        for (j = 0; j < n; j++)
        {
            *dptr2 = *dptr1;
            dptr2++; dptr1++;
        }
        dptr3 = PhiQ+n*n*(i-1); dptr2 = dptr2-n;
        F77_CALL(dtrsv)("l","n","n",&n,dptr3,&n,dptr2,&ione);
        F77_CALL(dtrsv)("l","t","n",&n,dptr3,&n,dptr2,&ione);
        F77_CALL(dgemv)("n",&n,&n,&fmone,A,&n,temp,&ione,&fone,dptr,&ione);
    }

    /* form be */
    dptr = be; dptr1 = rp; dptr2 = CPhiinvrd;
    for (i = 0; i < n*T; i++)
    {
        *dptr = (*dptr2)-(*dptr1);
        dptr++; dptr1++; dptr2++;
    }

    /* solve for dnu */
    dptr = v; dptr1 = be;
    for (i = 0; i < n; i++)
    {
        *dptr = -(*dptr1);
        dptr++; dptr1++;
    }
    dptr = dptr-n;
    F77_CALL(dtrsv)("l","n","n",&n,Ld,&n,dptr,&ione);
    for (i = 1; i < T; i++)
    {
        dptr = v+i*n; dptr1 = v+(i-1)*n; dptr2 = be+i*n; 
        for (j = 0; j < n; j++)
        {
            *dptr = *dptr2;
            dptr++; dptr2++;
        }
        dptr = dptr-n; dptr3 = Lld+n*n*(i-1);
        F77_CALL(dgemv)("n",&n,&n,&fmone,dptr3,&n,dptr1,&ione,&fmone,dptr,&ione);
        dptr3 = Ld+n*n*i;
        F77_CALL(dtrsv)("l","n","n",&n,dptr3,&n,dptr,&ione);
    }
    dptr = dnu+n*(T-1); dptr1 = v+n*(T-1);
    for (i = 0; i < n; i++)
    {
        *dptr = *dptr1;
        dptr++; dptr1++;
    }
    dptr = dptr-n; dptr3 = Ld+n*n*(T-1);
    F77_CALL(dtrsv)("l","t","n",&n,dptr3,&n,dptr,&ione);
    for (i = T-1; i > 0; i--)
    {
        dptr = dnu+n*(i-1); dptr1 = dnu+n*i; dptr2 = v+n*(i-1); 
        for (j = 0; j < n; j++)
        {
            *dptr = *dptr2;
            dptr++; dptr2++;
        }
        dptr = dptr-n; dptr3 = Lld+n*n*(i-1);
        F77_CALL(dgemv)("t",&n,&n,&fmone,dptr3,&n,dptr1,&ione,&fone,dptr,&ione);
        dptr3 = Ld+n*n*(i-1);
        F77_CALL(dtrsv)("l","t","n",&n,dptr3,&n,dptr,&ione);
    }

    /* form Ctdnu */
    for (i = 0; i < T-1; i++)
    {
        dptr = Ctdnu+i*(n+m); dptr1 = dnu+i*n;
        F77_CALL(dgemv)("n",&m,&n,&fmone,Bt,&m,dptr1,&ione,&fzero,dptr,&ione);
        dptr = Ctdnu+i*(n+m)+m; dptr2 = dnu+(i+1)*n;
        for (j = 0; j < n; j++)
        {
            *dptr = *dptr1;
            dptr++; dptr1++;
        }
        dptr = dptr-n;
        F77_CALL(dgemv)("n",&n,&n,&fmone,At,&n,dptr2,&ione,&fone,dptr,&ione);
    }
    
    dptr = Ctdnu+(T-1)*(n+m); dptr1 = dnu+(T-1)*n;
    F77_CALL(dgemv)("n",&m,&n,&fmone,Bt,&m,dptr1,&ione,&fzero,dptr,&ione);
    dptr = dptr+m; 
    for (i = 0; i < n; i++)
    {
        *dptr = *dptr1;
        dptr++; dptr1++;
    }
    dptr = rdmCtdnu; dptr1 = Ctdnu; dptr2 = rd;
    for (i = 0; i < nz; i++)
    {
        *dptr = -(*dptr1)-(*dptr2);
        dptr++; dptr1++; dptr2++;
    }

    /* solve for dz */
    for (i = 0; i < T; i++)
    {
        dptr = dz+(i+1)*m+i*n; dptr1 = rdmCtdnu+(i+1)*m+i*n;
        dptr2 = PhiinvQeye+n*n*i;
        F77_CALL(dgemv)("n",&n,&n,&fone,dptr2,&n,dptr1,&ione,&fzero,dptr,&ione);
    }
    for (i = 0; i < T; i++)
    {
        dptr = dz+i*(m+n); dptr1 = rdmCtdnu+i*(m+n);
        dptr2 = PhiinvReye+m*m*i;
        F77_CALL(dgemv)("n",&m,&m,&fone,dptr2,&m,dptr1,&ione,&fzero,dptr,&ione);
    }
    free(PhiQ); free(PhiR); free(PhiinvQAt); free(PhiinvRBt); free(PhiinvQeye);
    free(PhiinvReye); free(CPhiinvrd); free(Yd); free(Yud); free(Ld); free(Lld);
    free(gam); free(v); free(be); free(temp); free(tempmatn); free(tempmatm); 
    free(Ctdnu); free(rdmCtdnu);
    return;
}

/* computes rd and rp */
void rdrp(double *A, double *B, double *Q, double *R, double *Qf, double *z, double *nu, 
        double *gf, double *gp, double *b, int T, int n, int m, int nz, 
        double kappa, double *rd, double *rp, double *Ctnu)
{
    int i, j;
    double *Cz;
    double *dptr, *dptr1, *dptr2;

    Cz = malloc(sizeof(double)*T*n);
    
    /* compute Cz */
    dptr = Cz; dptr1 = z+m;
    for (i = 0; i < n; i++)
    {
        *dptr = *dptr1;
        dptr++; dptr1++;
    }
    F77_CALL(dgemv)("n",&n,&m,&fmone,B,&n,z,&ione,&fone,Cz,&ione);
    for (i = 2; i <= T; i++)
    {
        dptr = Cz+(i-1)*n; dptr1 = z+m+(i-2)*(n+m); 
        dptr2 = z+m+(i-1)*(m+n);
        for (j = 0; j < n; j++)
        {
            *dptr = *dptr2;
            dptr++; dptr2++;
        }
        dptr = dptr-n; 
        F77_CALL(dgemv)("n",&n,&n,&fmone,A,&n,dptr1,&ione,&fone,dptr,&ione);
        dptr1 = dptr1+n;
        F77_CALL(dgemv)("n",&n,&m,&fmone,B,&n,dptr1,&ione,&fone,dptr,&ione);
    }
    /*
    dptr = Cz+(T-1)*n; dptr1 = z+m+(T-2)*(n+m);
    F77_CALL(dgemv)("n",&n,&n,&fmone,A,&n,dptr1,&ione,&fzero,dptr,&ione);
    dptr1 = dptr1+n;
    F77_CALL(dgemv)("n",&n,&m,&fmone,B,&n,dptr1,&ione,&fone,dptr,&ione);
    dptr1 = z+nz-n;
    for (i = 0; i < n; i++)
    {
        *dptr = *dptr+*dptr1;
        dptr++; dptr1++;
    }
    */

    /* compute Ctnu */
    dptr = Ctnu; dptr1 = Ctnu+m; dptr2 = nu;
    for (i = 1; i <= T-1; i++)
    {
        F77_CALL(dgemv)("t",&n,&m,&fmone,B,&n,dptr2,&ione,&fzero,dptr,&ione);
        dptr = dptr+n+m;
        for (j = 0; j < n; j++)
        {
            *dptr1 = *dptr2;
            dptr1++; dptr2++;
        }
        dptr1 = Ctnu+m+(i-1)*(n+m);
        F77_CALL(dgemv)("t",&n,&n,&fmone,A,&n,dptr2,&ione,&fone,dptr1,&ione);
        dptr1 = dptr1+n+m;
    }
    F77_CALL(dgemv)("t",&n,&m,&fmone,B,&n,dptr2,&ione,&fzero,dptr,&ione);
    dptr = Ctnu+nz-n; dptr1 = nu+(T-1)*n;
    for (i = 0; i < n; i++)
    {
        *dptr = *dptr1;
        dptr++; dptr1++;
    }

    dptr = rp; dptr1 = Cz; dptr2 = b;
    for (i = 0; i < n*T; i++)
    {
        *dptr = *dptr1-*dptr2;
        dptr++; dptr1++; dptr2++;
    }
    dptr = rd; dptr1 = Ctnu; dptr2 = gf;
    for (i = 0; i < nz; i++)
    {
        *dptr = *dptr1+*dptr2;
        dptr++; dptr1++; dptr2++;
    }
    F77_CALL(daxpy)(&nz,&kappa,gp,&ione,rd,&ione);
    free(Cz);
    return;
}

/* computes gf, gp and hp */
void gfgphp(double *Q, double *R, double *Qf, double *zmax, double *zmin, double *z,
        int T, int n, int m, int nz, double *gf, double *gp, double *hp)
{
    int i;
    double *dptr, *dptr1, *dptr2;
    double *gp1, *gp2;

    gp1 = malloc(sizeof(double)*nz);
    gp2 = malloc(sizeof(double)*nz);

    dptr = gp1; dptr1 = zmax; dptr2 = z;
    for (i = 0; i < nz; i++)
    {
        *dptr = 1.0/(*dptr1-*dptr2);
        dptr++; dptr1++; dptr2++;
    }
    dptr = gp2; dptr1 = zmin; dptr2 = z;
    for (i = 0; i < nz; i++)
    {
        *dptr = 1.0/(*dptr2-*dptr1);
        dptr++; dptr1++; dptr2++;
    }
    dptr = hp; dptr1 = gp1; dptr2 = gp2;
    for (i = 0; i < nz; i++)
    {
        *dptr = (*dptr1)*(*dptr1) + (*dptr2)*(*dptr2);
        dptr++; dptr1++; dptr2++;
    }
    dptr = gp; dptr1 = gp1; dptr2 = gp2;
    for (i = 0; i < nz; i++)
    {
        *dptr = *dptr1-*dptr2;
        dptr++; dptr1++; dptr2++;
    }
    
    dptr = gf; dptr1 = z; 
    for (i = 0; i < T-1; i++)
    {
        F77_CALL(dgemv)("n",&m,&m,&ftwo,R,&m,dptr1,&ione,&fzero,dptr,&ione);
        dptr = dptr+m; dptr1 = dptr1+m;
        F77_CALL(dgemv)("n",&n,&n,&ftwo,Q,&n,dptr1,&ione,&fzero,dptr,&ione);
        dptr = dptr+n; dptr1 = dptr1+n;
    }
    F77_CALL(dgemv)("n",&m,&m,&ftwo,R,&m,dptr1,&ione,&fzero,dptr,&ione);
    dptr = dptr+m; dptr1 = dptr1+m;
    F77_CALL(dgemv)("n",&n,&n,&ftwo,Qf,&n,dptr1,&ione,&fzero,dptr,&ione);

    free(gp1); free(gp2);
    return;
}

/* computes resd, resp, and res */
void resdresp(double *rd, double *rp, int T, int n, int nz, double *resd, 
        double *resp, double *res)
{
    int nnu = T*n;
    *resp = F77_CALL(dnrm2)(&nnu,rp,&ione);
    *resd = F77_CALL(dnrm2)(&nz,rd,&ione);
    *res = sqrt((*resp)*(*resp)+(*resd)*(*resd));
    return;
}


				
// Read in PWM value
/*
	printf("Enter PWM value:\n");
	fgets(input, 10, stdin);
	value[0] = atof(input);
	printf("Value: %f\n", value[0]);
	
	for (int i=1;i<4;i++){
		value[i]=value[0];
	}
*/	
