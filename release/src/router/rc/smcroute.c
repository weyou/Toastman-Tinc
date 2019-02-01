/*

    Copyright (C) 2018 Kelvin You
    weyou.dev@gmail.com

*/

#include "rc.h"

#define BUF_SIZE 256

void start_smcroute(void)
{

    const char *tmp_value;
    FILE *fp, *hp;

    // write smcroute configuration file
    if ( strcmp( tmp_value = nvram_safe_get("smc_config"), "") != 0 ){
        if ( !( fp = fopen( "/etc/smcroute.conf", "w" ))){
            perror( "/etc/smcroute.conf" );
            return;
        }
        fprintf(fp, "%s\n", tmp_value);
        fclose(fp);
        chmod("/etc/smcroute.conf", 0600);
    }

    // Create firewall script.
    if ( !( hp = fopen( "/etc/smcroute-fw.sh", "w" ))){
        perror( "/etc/smcroute-fw.sh" );
        return;
    }

    fprintf(hp, "#!/bin/sh\n" );
    if ( strcmp( tmp_value = nvram_safe_get("smc_firewall"), "") != 0 ){
        fprintf(hp, "\n" );
        fprintf(hp, "%s\n", tmp_value );
    }

    fclose(hp);
    chmod("/etc/smcroute-fw.sh", 0744);

    run_smcroute_firewall_script();
    xstart( "/usr/sbin/smcrouted", "-N", "-d", "10");
    return;
}

void stop_smcroute(void)
{
    killall("smcrouted", SIGTERM);
    run_smcroute_firewall_script();
    system( "/bin/rm -rf /etc/smcroute.conf\n" );
    return;
}

void run_smcroute_firewall_script(void){
    FILE *fp;

    if ((fp = fopen( "/etc/smcroute-fw.sh", "r" ))){

        fclose(fp);
        system( "/etc/smcroute-fw.sh" );
    }

    return;
}

void start_smcroute_wanup(void){

    if ( nvram_match("smc_enable", "1") )
        start_smcroute();

    return;
}
