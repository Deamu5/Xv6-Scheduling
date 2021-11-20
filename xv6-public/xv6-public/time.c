#include "types.h"
#include "fcntl.h"
#include "stat.h"
#include "user.h"

int main(int argc,char** argv){
	long long int n=1;
	if(argc<2)
		n=1;	//default value
	else
	n=atoi(argv[1]);
	volatile int x;
	int pid;
	int runvar;
	int waitvar;
	long long int ind=0;
	while(ind<n){
		pid=fork();
		if(pid<0){
			printf(1,"fork failed\n");
		}
		else if(pid>0){
			printf(1,"Parent %d created child %d\n",getpid(),pid);
			waitx(&waitvar,&runvar);
			printf(1,"pid:%d waittime:%d runtime:%d\n",pid,waitvar,runvar);
		}
		else{
			long long int r=0;
			while(r<100000000){
				x=x+5;
				x*=5.344;
				x/=6.2442;
				x-=3853;
			r++;
			}
			break;
		}
	ind++;
	}
	exit();
}