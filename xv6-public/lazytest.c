#include "types.h"
#include "stat.h"
#include "user.h"

int
main(int argc, char *argv[]) {
  
  
  //sleep(100);
  //exit();
      
  
  int pid[3];
  int i;
  int count=0;
  for(i = 0; i < 3; i++) 
      pid[i] = fork();       
  for(i = 0; i < 3; i++)        
    if(pid[i] == 0) 
      {
        sleep(100);
        exit();
      }
  //while(1)
    for(i = 0; i < 3; i++)
      if(pid[i] > 0) 
      {
        if(pid[i] == wait())
          count++;// successfully retrieve +1
        //if(count==3)// For fork reason, there are ? children actually
         // exit();  
      }
      exit();
}
