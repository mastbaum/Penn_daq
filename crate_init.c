#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "penn_daq.h"
#include "fec_util.h"
#include "net_util.h"
//#include "pouch.h"
//#include "json.h"
//#include "db.h"


int crate_init(char *buffer)
{
    char *words,*words2;
    int crate_num=2;
    uint32_t xilinx_load=0,hv_reset=0;
    uint32_t slot_mask=0x80;
    int use_cbal=0,use_zdisc=0,use_ttot=0,use_all=0,use_hw=0;
    int result;

    int i,j,k,l;
    int crate,card;
    XL3_Packet packet;
    uint32_t *mb_num;
    char get_db_address[500];
    char ctc_address[500];
    hware_vals_t *hware_flip;


    // lets get the parameters from command line
    words = strtok(buffer, " ");
    while (words != NULL)
    {
        if (words[0] == '-')
        {
            if (words[1] == 'c'){
                words2 = strtok(NULL, " ");
                crate_num=atoi(words2);
            }else if (words[1] == 's'){
                words2 = strtok(NULL, " ");
                slot_mask = strtoul(words2,(char**)NULL,16);
            }else if (words[1] == 'x'){xilinx_load = 1;
            }else if (words[1] == 'X'){xilinx_load = 2;
            }else if (words[1] == 'v'){hv_reset = 1;
            }else if (words[1] == 'b'){use_cbal = 1;
            }else if (words[1] == 'd'){use_zdisc = 1;
            }else if (words[1] == 't'){use_ttot = 1;
            }else if (words[1] == 'a'){use_all = 1;
            }else if (words[1] == 'w'){use_hw = 1;
            }else if (words[1] == 'h'){
                printsend( "Usage: crate_init -c [crate_num] -s [slot_mask]\n");
                printsend(
                        "		  -x (load xilinx) -X (load cald xilinx) -v (reset HV dac)\n");
                printsend(
                        "		  -b (load cbal from db) -d (load zdisc from db) -t (load ttot from db) -a (load all from db) -w (use crate/card specific values from db)\n");
                return 0;
            }
        }
        words = strtok(NULL, " ");
    }

    if (connected_xl3s[crate_num] == -999){
        printsend( "Crate %d's XL3 is not connected\n",crate_num);
        return -1;
    }

    printsend( "Initializing crate %d, slots %08x, xl:%d, hv:%d\n",crate_num,slot_mask,xilinx_load,hv_reset);

    printsend("Sending database to XL3s\n");

    pouch_request *hw_response = pr_init();
    JsonNode* hw_rows = NULL;
    pouch_request *debug_response = pr_init();
    JsonNode* debug_doc = NULL;

    if (use_hw == 1){
        sprintf(get_db_address,"http://%s:%s/%s/%s/get_fec?startkey=[%d,0]&endkey=[%d,15]",DB_ADDRESS,DB_PORT,DB_BASE_NAME,DB_VIEWDOC,crate_num,crate_num);
        pr_set_method(hw_response, GET);
        pr_set_url(hw_response, get_db_address);
        pr_do(hw_response);
        if (hw_response->httpresponse != 200){
           printsend("Unable to connect to database. error code %d\n",(int)hw_response->httpresponse);
            return -1;
        }
        JsonNode *hw_doc = json_decode(hw_response->resp.data);
        JsonNode* totalrows = json_find_member(hw_doc,"total_rows");
        if ((int)json_get_number(totalrows) != 16){
           printsend("Database error: not enough FEC entries\n");
            return -1;
        }
        hw_rows = json_find_member(hw_doc,"rows");
        //json_delete(hw_doc); // only delete the head node
        pr_free(hw_response);
    }else{
        sprintf(get_db_address,"http://%s:%s/%s/CRATE_INIT_DOC",DB_ADDRESS,DB_PORT,DB_BASE_NAME);
        pr_set_method(debug_response, GET);
        pr_set_url(debug_response, get_db_address);
        pr_do(debug_response);
        if (debug_response->httpresponse != 200){
           printsend("Unable to connect to database. error code %d\n",(int)debug_response->httpresponse);
            return -1;
        }
        debug_doc = json_decode(debug_response->resp.data);
        pr_free(debug_response);
    }

    mb_num = (uint32_t *) packet.payload;

    // make sure crate_config is up to date
    if (use_cbal || use_zdisc || use_ttot || use_all)
        update_crate_config(crate_num,slot_mask);

    // GET ALL FEC DATA FROM DB
    for (i=0;i<16;i++){

        mb_t* mb_consts = (mb_t *) (packet.payload+4);
        packet.cmdHeader.packet_type = CRATE_INIT_ID;

        ///////////////////////////
        // GET DEFAULT DB VALUES //
        ///////////////////////////

        if (use_hw == 1){
            JsonNode* next_row = json_find_element(hw_rows,i);
            JsonNode* key = json_find_member(next_row,"key");
            JsonNode* value = json_find_member(next_row,"value");
            JsonNode* hw = json_find_member(value,"hw");
            crate = (int)json_get_number(json_find_element(key,0));
            card = (int)json_get_number(json_find_element(key,1));
            if (crate != crate_num || card != i){
               printsend("Database error : incorrect crate or card num (%d,%d)\n",crate,card);
                return -1;
            }
            parse_fec_hw(value,mb_consts);
        }else{
            parse_fec_debug(debug_doc,mb_consts);
        }

        //////////////////////////////
        // GET VALUES FROM DEBUG DB //
        //////////////////////////////

        if ((use_cbal || use_all) && ((0x1<<i) & slot_mask)){
            if (crate_config[crate_num][i].mb_id == 0x0000){
               printsend("Warning: mb_id unknown. Using default values. Make sure to load xilinx before attempting to use debug db values.\n");
            }else{
                char config_string[500];
                sprintf(config_string,"\"%04x\",\"%04x\",\"%04x\",\"%04x\",\"%04x\"",crate_config[crate_num][i].mb_id,crate_config[crate_num][i].dc_id[0],crate_config[crate_num][i].dc_id[1],crate_config[crate_num][i].dc_id[2],crate_config[crate_num][i].dc_id[3]);
                sprintf(get_db_address,"http://%s:%s/%s/%s/get_crate_cbal?startkey=[%s,9999999999]&endkey=[%s,0]&descending=true",DB_ADDRESS,DB_PORT,DB_BASE_NAME,DB_VIEWDOC,config_string,config_string);
                pouch_request *cbal_response = pr_init();
                pr_set_method(cbal_response, GET);
                pr_set_url(cbal_response, get_db_address);
                pr_do(cbal_response);
                if (cbal_response->httpresponse != 200){
                   printsend("Unable to connect to database. error code %d\n",(int)cbal_response->httpresponse);
                    return -1;
                }
                JsonNode *viewdoc = json_decode(cbal_response->resp.data);
                JsonNode* viewrows = json_find_member(viewdoc,"rows");
                int n = json_get_num_mems(viewrows);
                if (n == 0){
                   printsend("No crate_cbal documents for this configuration (%s). Continuing with default values.\n",config_string);
                }else{
                    // these next three JSON nodes are pointers to the structure of viewrows; no need to delete
                    JsonNode* cbal_doc = json_find_member(json_find_element(viewrows,0),"value");
                    JsonNode* vbal_low = json_find_member(cbal_doc,"vbal_low");
                    JsonNode* vbal_high = json_find_member(cbal_doc,"vbal_high");
                    for (j=0;j<32;j++){
                        mb_consts->vbal[0][j] = (int)json_get_number(json_find_element(vbal_low,j));
                        mb_consts->vbal[1][j] = (int)json_get_number(json_find_element(vbal_high,j));
                    }
                }
                json_delete(viewdoc); // viewrows is part of viewdoc; only delete the head node
                pr_free(cbal_response);
            }
        }

        if ((use_zdisc || use_all) && ((0x1<<i) & slot_mask)){
            if (crate_config[crate_num][i].mb_id == 0x0000){
            }else{
                char config_string[500];
                sprintf(config_string,"\"%04x\",\"%04x\",\"%04x\",\"%04x\",\"%04x\"",crate_config[crate_num][i].mb_id,crate_config[crate_num][i].dc_id[0],crate_config[crate_num][i].dc_id[1],crate_config[crate_num][i].dc_id[2],crate_config[crate_num][i].dc_id[3]);
                sprintf(get_db_address,"http://%s:%s/%s/%s/get_zdisc?startkey=[%s,9999999999]&endkey=[%s,0]&descending=true",DB_ADDRESS,DB_PORT,DB_BASE_NAME,DB_VIEWDOC,config_string,config_string);
                pouch_request *zdisc_response = pr_init();
                pr_set_method(zdisc_response, GET);
                pr_set_url(zdisc_response, get_db_address);
                pr_do(zdisc_response);
                if (zdisc_response->httpresponse != 200){
                   printsend("Unable to connect to database. error code %d\n",(int)zdisc_response->httpresponse);
                    return -1;
                }
                JsonNode *viewdoc = json_decode(zdisc_response->resp.data);
                JsonNode* viewrows = json_find_member(viewdoc,"rows");
                int n = json_get_num_mems(viewrows);
                if (n == 0){
                   printsend("No zdisc documents for this configuration (%s). Continuing with default values.\n",config_string);
                }else{
                    JsonNode* zdisc_doc = json_find_member(json_find_element(viewrows,0),"value");
                    JsonNode* vthr = json_find_member(zdisc_doc,"Zero_Dac_setting");
                    for (j=0;j<32;j++){
                        mb_consts->vthr[j] = (int)json_get_number(json_find_element(vthr,j));
                    }
                }
                json_delete(viewdoc); // only delete the head
                pr_free(zdisc_response);
            }
        }


        if ((use_ttot || use_all) && ((0x1<<i) & slot_mask)){
            if (crate_config[crate_num][i].mb_id == 0x0000){
               printsend("Warning: mb_id unknown. Using default values. Make sure to load xilinx before attempting to use debug db values.\n");
            }else{
                char config_string[500];
                sprintf(config_string,"\"%04x\",\"%04x\",\"%04x\",\"%04x\",\"%04x\"",crate_config[crate_num][i].mb_id,crate_config[crate_num][i].dc_id[0],crate_config[crate_num][i].dc_id[1],crate_config[crate_num][i].dc_id[2],crate_config[crate_num][i].dc_id[3]);
                sprintf(get_db_address,"http://%s:%s/%s/%s/get_ttot?startkey=[%s,9999999999]&endkey=[%s,0]&descending=true",DB_ADDRESS,DB_PORT,DB_BASE_NAME,DB_VIEWDOC,config_string,config_string);
                pouch_request *ttot_response = pr_init();
                pr_set_method(ttot_response, GET);
                pr_set_url(ttot_response, get_db_address);
                pr_do(ttot_response);
                if (ttot_response->httpresponse != 200){
                   printsend("Unable to connect to database. error code %d\n",(int)ttot_response->httpresponse);
                    return -1;
                }
                JsonNode *viewdoc = json_decode(ttot_response->resp.data);
                JsonNode* viewrows = json_find_member(viewdoc,"rows");
                int n = json_get_num_mems(viewrows);
                if (n == 0){
                   printsend("No set_ttot documents for this configuration (%s). Continuing with default values.\n",config_string);
                }else{
                    JsonNode* ttot_doc = json_find_member(json_find_element(viewrows,0),"value");
                    JsonNode* rmp = json_find_member(ttot_doc,"rmp");
                    JsonNode* vsi = json_find_member(ttot_doc,"vsi");
                    for (j=0;j<8;j++){
                        mb_consts->tdisc.rmp[j] = (int)json_get_number(json_find_element(rmp,j));
                        mb_consts->tdisc.vsi[j] = (int)json_get_number(json_find_element(vsi,j));
                    }
                }
                json_delete(viewdoc);
                pr_free(ttot_response);
            }
        }


        ///////////////////////////////
        // SEND THE DATABASE TO XL3s //
        ///////////////////////////////

        *mb_num = i;

        SwapLongBlock(&(packet.payload),1);	
        swap_fec_db(mb_consts);
        do_xl3_cmd_no_response(&packet,crate_num);

    }

    // GET CTC DELAY FROM CTC_DOC IN DB
    pouch_request *ctc_response = pr_init();
    sprintf(ctc_address,"http://%s:%s/%s/CTC_doc",DB_ADDRESS,DB_PORT,DB_BASE_NAME);
    pr_set_method(ctc_response, GET);
    pr_set_url(ctc_response, ctc_address);
    pr_do(ctc_response);
    if (ctc_response->httpresponse != 200){
       printsend("Error getting ctc document, error code %d\n",(int)ctc_response->httpresponse);
        return -1;
    }
    JsonNode *ctc_doc = json_decode(ctc_response->resp.data);
    JsonNode *ctc_delay_a = json_find_member(ctc_doc,"delay");
    uint32_t ctc_delay = strtoul(json_get_string(json_find_element(ctc_delay_a,crate_num)),(char**) NULL,16);
    json_delete(ctc_doc); // delete the head node
    pr_free(ctc_response);


    // START CRATE_INIT ON ML403
    printsend("Beginning crate_init.\n");

    packet.cmdHeader.packet_type = CRATE_INIT_ID;
    *mb_num = 666;

    uint32_t *p;
    p = (uint32_t *) (packet.payload+4);
    *p = xilinx_load;
    p++;
    *p = hv_reset;
    p++;
    *p = slot_mask;
    p++;
    *p = ctc_delay;
    p++;
    *p = 0x0;

    SwapLongBlock(&(packet.payload),5);

    do_xl3_cmd(&packet,crate_num); 

    // NOW PROCESS RESULTS AND POST TO DB
    hware_flip = (hware_vals_t *) (packet.payload+4);

    for (i=0;i<16;i++){
        SwapShortBlock(&(hware_flip->mb_id),1);
        SwapShortBlock(&(hware_flip->dc_id),4);
        hware_flip++;
    }

    //update_crate_configuration(crate_num,hware_flip);  //FIXME
    printsend("Crate configuration updated.\n");
    printsend("*******************************\n");
    json_delete(hw_rows);
    json_delete(debug_doc);
    return 0;
}
