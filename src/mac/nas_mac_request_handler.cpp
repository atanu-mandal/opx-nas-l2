/*
 * Copyright (c) 2016 Dell Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License. You may obtain
 * a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
 *
 * THIS CODE IS PROVIDED ON AN *AS IS* BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT
 * LIMITATION ANY IMPLIED WARRANTIES OR CONDITIONS OF TITLE, FITNESS
 * FOR A PARTICULAR PURPOSE, MERCHANTABLITY OR NON-INFRINGEMENT.
 *
 * See the Apache Version 2.0 License for specific language governing
 * permissions and limitations under the License.
 */

#include "nas_mac_api.h"
#include "std_error_codes.h"
#include "hal_if_mapping.h"
#include "nas_ndi_mac.h"
#include "nas_if_utils.h"
#include "std_socket_tools.h"
#include "std_file_utils.h"
#include "std_select_tools.h"
#include "cps_api_operation.h"
#include "cps_class_map.h"
#include "cps_api_events.h"

#include <unistd.h>
#include <vector>
#include <mutex>
#include <condition_variable>


static auto nas_mac_request_queue = new nas_mac_cps_event_queue_t;
static auto mac_delete_queue = new nas_mac_cps_event_queue_t;
static auto nas_mac_event_queue =  *new nas_mac_npu_event_queue_t;
static size_t vlan_flush_count =0;
static size_t port_flush_count =0;
static size_t port_vlan_flush_count  = 0;
static bool clear_all=false;
static std::mutex mac_npu_event_mtx;
static auto port_flush_queue = new std::unordered_map <hal_ifindex_t, nas_mac_cps_event_t>;
static auto vlan_flush_queue = new std::unordered_map <hal_vlan_id_t, nas_mac_cps_event_t>;

typedef struct port_vlan_flush{
    hal_ifindex_t ifindex;
    hal_vlan_id_t vlan_id;

    bool operator == (port_vlan_flush const & rhs) const{
        if(ifindex != rhs.ifindex) return false;
        if(vlan_id != rhs.vlan_id) return false;
        return true;
    }
}port_vlan_flush_t;


struct port_vlan_flush_hash{
     std::size_t operator() (port_vlan_flush_t const & entry) const {
        std::size_t ifindex_hash = std::hash<unsigned int>()(entry.ifindex);
        std::size_t vlan_hash = std::hash<unsigned int>()(entry.vlan_id);
        return (ifindex_hash<<1)^(vlan_hash <<1);
    }
};

static auto port_vlan_flush_queue = new std::unordered_map<port_vlan_flush_t,
                                    nas_mac_cps_event_t, port_vlan_flush_hash>;

static cps_api_event_service_handle_t handle;
static unsigned int max_pub_thresold = 40;

t_std_error nas_mac_event_handle_init(){

    if (cps_api_event_service_init() != cps_api_ret_code_OK) {
        return false;
    }

    if (cps_api_event_client_connect(&handle) != cps_api_ret_code_OK) {
        return false;
    }

    return STD_ERR_OK;
}


t_std_error nas_mac_event_publish(cps_api_object_t obj){
    cps_api_return_code_t rc;
    if((rc = cps_api_event_publish(handle,obj))!= cps_api_ret_code_OK){
        EV_LOGGING(L2MAC,ERR,"MAC-EV-PUB","Failed to publish MAC event");
        cps_api_object_delete(obj);
        return (t_std_error)rc;
    }
    cps_api_object_delete(obj);
    return STD_ERR_OK;
}

static auto nas_to_pub_ev_type = new std::unordered_map<unsigned int,unsigned int>{
    {NAS_MAC_ADD,BASE_MAC_MAC_EVENT_TYPE_LEARNT},
    {NAS_MAC_DEL,BASE_MAC_MAC_EVENT_TYPE_AGED},
    {NAS_MAC_FLUSH,BASE_MAC_MAC_EVENT_TYPE_FLUSHED},
    {NAS_MAC_MOVE,BASE_MAC_MAC_EVENT_TYPE_MOVED}
};

void nas_mac_add_event_entry_to_obj(cps_api_object_t obj,nas_mac_entry_t & entry,nas_l2_mac_op_t add, unsigned int index){

    BASE_MAC_MAC_EVENT_TYPE_t ev_type;
    auto it = nas_to_pub_ev_type->find(add);

    if(it != nas_to_pub_ev_type->end()){
        ev_type = (BASE_MAC_MAC_EVENT_TYPE_t)it->second;
    }else{
        NAS_MAC_LOG(ERR,"Invalid nas l2 mac op %d for publishing events",add);
        return;
    }

    if(ev_type == BASE_MAC_MAC_EVENT_TYPE_AGED){
        nas_mac_update_entry_in_os(&entry,cps_api_oper_DELETE);
    }else if (ev_type == BASE_MAC_MAC_EVENT_TYPE_MOVED){
        nas_mac_update_entry_in_os(&entry,cps_api_oper_SET);
    }

    cps_api_attr_id_t ids[3] = {BASE_MAC_LIST_ENTRIES, index,BASE_MAC_LIST_ENTRIES_IFINDEX };
    const int ids_len = sizeof(ids)/sizeof(ids[0]);
    cps_api_object_e_add(obj,ids,ids_len,cps_api_object_ATTR_T_U32,&entry.ifindex,sizeof(entry.ifindex));
    ids[2] = BASE_MAC_LIST_ENTRIES_ACTIONS;
    cps_api_object_e_add(obj,ids,ids_len,cps_api_object_ATTR_T_U32,&entry.pkt_action,sizeof(entry.pkt_action));
    ids[2] = BASE_MAC_LIST_ENTRIES_VLAN;
    cps_api_object_e_add(obj,ids,ids_len,cps_api_object_ATTR_T_U16,&entry.entry_key.vlan_id,
                     sizeof(entry.entry_key.vlan_id));
    ids[2] = BASE_MAC_LIST_ENTRIES_MAC_ADDRESS;
    cps_api_object_e_add(obj,ids,ids_len,cps_api_object_ATTR_T_BIN,(void*)&entry.entry_key.mac_addr,
         sizeof(entry.entry_key.mac_addr));
    ids[2] = BASE_MAC_LIST_ENTRIES_STATIC;
    cps_api_object_e_add(obj,ids,ids_len,cps_api_object_ATTR_T_BIN,&entry.is_static,sizeof(entry.is_static));

    ids[2] = BASE_MAC_LIST_ENTRIES_EVENT_TYPE;
    cps_api_object_e_add(obj,ids,ids_len,cps_api_object_ATTR_T_U32,&ev_type,sizeof(ev_type));


    interface_ctrl_t intf_ctrl;
    memset(&intf_ctrl, 0, sizeof(interface_ctrl_t));

    intf_ctrl.q_type = HAL_INTF_INFO_FROM_IF;
    intf_ctrl.if_index = entry.ifindex;

    if(dn_hal_get_interface_info(&intf_ctrl) == STD_ERR_OK) {
     ids[2] = BASE_MAC_LIST_ENTRIES_IFNAME;
     cps_api_object_e_add(obj,ids ,ids_len,cps_api_object_ATTR_T_BIN, (const void *)intf_ctrl.if_name,
                                strlen(intf_ctrl.if_name)+1);
    }

}


static bool nas_mac_process_pub_queue(){

    cps_api_key_t key;
    cps_api_key_from_attr_with_qual(&key, BASE_MAC_LIST_OBJ,
                                       cps_api_qualifier_OBSERVED);
    unsigned int entry_count = 0;
    while(nas_mac_event_queue.size() > 0){
        cps_api_object_t obj = cps_api_object_create();
        if(obj == nullptr){
            NAS_MAC_LOG(ERR,"Failed to allocate memory for mac event publish");
            return false;
        }
        cps_api_object_set_key(obj,&key);
        while(entry_count < max_pub_thresold && nas_mac_event_queue.size() > 0){
            nas_mac_npu_event_t & event = nas_mac_event_queue.front();
            nas_mac_add_event_entry_to_obj(obj,event.entry,event.op_type,entry_count++);
            nas_mac_event_queue.pop_front();
        }
        entry_count=0;
        nas_mac_event_publish(obj);
    }
    return true;
}
void nas_mac_flush_count_dump(void){
    EV_LOGGING(L2MAC,NOTICE,"FLUSH-COUNT","Port flush count %lu",port_flush_count);
    EV_LOGGING(L2MAC,NOTICE,"FLUSH-COUNT","vlan flush count %lu",vlan_flush_count);
    EV_LOGGING(L2MAC,NOTICE,"FLUSH-COUNT","Port vlan flush count %lu",port_vlan_flush_count);
    EV_LOGGING(L2MAC,NOTICE,"FLUSH-COUNT","Total flush count %lu",(port_flush_count+vlan_flush_count+port_vlan_flush_count));
}

static t_std_error nas_mac_read_cps_notification(int fd, const size_t count,nas_mac_cps_event_queue_t & flush_list){

    t_std_error rc;
    int len = sizeof(nas_mac_cps_event_t) * count;
    std::vector<nas_mac_cps_event_t> mac_cps_ev_vec;
    mac_cps_ev_vec.resize(count);

    if(std_read(fd,&mac_cps_ev_vec[0],len,true,&rc) != len){
        EV_LOGGING(L2MAC,ERR,"L2-FLUSH-NOT","Failed to read flush_req");
        return rc;
    }

    for(unsigned int ix = 0 ; ix < count ; ++ix){
        flush_list.push_back(mac_cps_ev_vec[ix]);
    }

    return STD_ERR_OK;
}


static t_std_error nas_mac_read_npu_notification(int fd, const size_t count){

    t_std_error rc;
    int len = sizeof(nas_mac_npu_event_t) * count;
    std::vector<nas_mac_npu_event_t> mac_npu_event_vec;
    mac_npu_event_vec.resize(count);

    if(std_read(fd,&mac_npu_event_vec[0],len,true,&rc) != len){
        EV_LOGGING(L2MAC,ERR,"L2-FLUSH-NOT","Failed to read npu notification");
        return rc;
    }

    std::lock_guard<std::mutex> lk(mac_npu_event_mtx);
    for(unsigned int ix = 0 ; ix < count ; ++ix){
        nas_mac_event_queue.push_back(mac_npu_event_vec[ix]);
    }
    nas_mac_process_pub_queue();

    return STD_ERR_OK;
}


t_std_error nas_mac_delete_entries_from_hw(nas_mac_entry_t *entry,
                                           ndi_mac_delete_type_t del_type,
                                           bool subtype_all) {
    t_std_error rc = STD_ERR_OK;
    ndi_obj_id_t obj_id;
    interface_ctrl_t intf_ctrl;
    memset(&intf_ctrl, 0, sizeof(interface_ctrl_t));
    intf_ctrl.q_type = HAL_INTF_INFO_FROM_IF;
    intf_ctrl.if_index = entry->ifindex;
    (void)subtype_all; // TODO Get the attribute from CPS

    ndi_mac_entry_t ndi_mac_entry;
    memset(&ndi_mac_entry, 0, sizeof(ndi_mac_entry_t));
    if(del_type == NDI_MAC_DEL_BY_PORT){
        port_flush_count +=1;
    }else if(del_type == NDI_MAC_DEL_BY_VLAN){
        vlan_flush_count +=1;
    }else if(del_type == NDI_MAC_DEL_BY_PORT_VLAN){
        port_vlan_flush_count +=1;
    }

    if ((del_type == NDI_MAC_DEL_BY_PORT) || (del_type == NDI_MAC_DEL_BY_PORT_VLAN))
    {
        if ((rc = dn_hal_get_interface_info(&intf_ctrl)) != STD_ERR_OK) {
             NAS_MAC_LOG(ERR, "Get interface info failed for if index 0x%x.", entry->ifindex);
             return rc;
        }
        if(intf_ctrl.int_type == nas_int_type_LAG)
        {
           if(nas_get_lag_id_from_if_index(entry->ifindex, &obj_id) == STD_ERR_OK)
           {
              ndi_mac_entry.ndi_lag_id = obj_id;
           }
        }
        else
        {
           ndi_mac_entry.npu_id = 0; /* TODO: Handle multiple NPU scenerio */
           ndi_mac_entry.port_info.npu_id = intf_ctrl.npu_id;
           ndi_mac_entry.port_info.npu_port = intf_ctrl.port_id;
        }
    }

    ndi_mac_entry.vlan_id = entry->entry_key.vlan_id;

    if (del_type == NDI_MAC_DEL_SINGLE_ENTRY) {
        memcpy(ndi_mac_entry.mac_addr, entry->entry_key.mac_addr, sizeof(hal_mac_addr_t));
    }
    ndi_mac_entry.is_static = entry->is_static;

    if ((rc = ndi_delete_mac_entry(&ndi_mac_entry, del_type, true) != STD_ERR_OK)) {
        NAS_MAC_LOG(ERR, "Error deleting NDI MAC entry with vlan id %d",
            entry->entry_key.vlan_id);
        return rc;
    }

    if(del_type != NDI_MAC_DEL_SINGLE_ENTRY){
        nas_mac_publish_flush_event(del_type,entry);
    }

    return STD_ERR_OK;
}


static void nas_mac_clear_hw_mac(nas_mac_cps_event_t & req_entry){

    if(nas_mac_delete_entries_from_hw(&(req_entry.entry),
                                req_entry.del_type,req_entry.subtype_all) != STD_ERR_OK){
        NAS_MAC_LOG(ERR,"Failed to remove MAC entry from hardware");
    }


    if(req_entry.del_type == NDI_MAC_DEL_ALL_ENTRIES){
        std::lock_guard<std::mutex> lk(mac_npu_event_mtx);
        nas_mac_event_queue.clear();
        return;
    }
}

static void nas_mac_compact_flush_requests(){
    if(clear_all){
        port_flush_queue->clear();
        vlan_flush_queue->clear();
        port_vlan_flush_queue->clear();
        clear_all = false;
    }

    for(auto it = port_vlan_flush_queue->begin(); it != port_vlan_flush_queue->end() ; ++it){
        if(vlan_flush_queue->find(it->first.vlan_id) == vlan_flush_queue->end() &&
           port_flush_queue->find(it->first.ifindex) == port_flush_queue->end()){
            nas_mac_clear_hw_mac(it->second);
        }
    }

    for(auto it = port_flush_queue->begin(); it != port_flush_queue->end() ; ++it){
        nas_mac_clear_hw_mac(it->second);
    }

    for(auto it = vlan_flush_queue->begin(); it != vlan_flush_queue->end(); ++it){
        nas_mac_clear_hw_mac(it->second);
    }

    for(auto it = mac_delete_queue->begin(); it != mac_delete_queue->end(); ++it){
        nas_mac_clear_hw_mac(*it);
    }
    mac_delete_queue->clear();
    port_flush_queue->clear();
    vlan_flush_queue->clear();
    port_vlan_flush_queue->clear();

}

static void nas_mac_drain_cps_queue(int fd,size_t count){
    if(nas_mac_read_cps_notification(fd,count,*nas_mac_request_queue)!=STD_ERR_OK){
        return;
    }

    while(!nas_mac_request_queue->empty()){
        nas_mac_cps_event_t & req_entry = nas_mac_request_queue->front();
        switch(req_entry.del_type){
        case NDI_MAC_DEL_BY_PORT:
            port_flush_queue->insert({req_entry.entry.ifindex, std::move(req_entry)});
            break;

        case NDI_MAC_DEL_BY_VLAN:
            vlan_flush_queue->insert({req_entry.entry.entry_key.vlan_id,std::move(req_entry)});
            break;

        case NDI_MAC_DEL_BY_PORT_VLAN:
        {
            port_vlan_flush_t pv;
            pv.ifindex=req_entry.entry.ifindex;
            pv.vlan_id=req_entry.entry.entry_key.vlan_id;
            port_vlan_flush_queue->insert({pv, std::move(req_entry)});
        }
            break;

        case NDI_MAC_DEL_ALL_ENTRIES:
            clear_all = true;
            mac_delete_queue->push_back(std::move(req_entry));
            break;

        case NDI_MAC_DEL_SINGLE_ENTRY:
            mac_delete_queue->push_back(std::move(req_entry));
            break;


        default:
            break;
        }
        if(!nas_mac_request_queue->empty()){
            nas_mac_request_queue->pop_front();
        }
    }

    nas_mac_compact_flush_requests();
}


static void nas_mac_process_cps_events(int fd){
    nas_l2_event_header_t ev_hdr;
    t_std_error rc;
    if(std_read(fd,&ev_hdr,sizeof(ev_hdr),true,&rc)!= sizeof(ev_hdr)){
        NAS_MAC_LOG(ERR,"Failed to read mac event header from fd %d",fd);
    }

    nas_mac_drain_cps_queue(fd,ev_hdr.len);

}

static void nas_mac_process_npu_events(int fd){
    nas_l2_event_header_t ev_hdr;
    t_std_error rc;
    if(std_read(fd,&ev_hdr,sizeof(ev_hdr),true,&rc)!= sizeof(ev_hdr)){
        NAS_MAC_LOG(ERR,"Failed to read mac event header from fd %d",fd);
    }

    nas_mac_read_npu_notification(fd,ev_hdr.len);

}


void nas_l2_mac_npu_req_handler(void){

    int npu_read_fd = nas_mac_get_read_npu_thread_fd();
    while(true){
        nas_mac_process_npu_events(npu_read_fd);
    }

}

void nas_l2_mac_cps_req_handler(void){

    int cps_read_fd = nas_mac_get_read_cps_thread_fd();
    while(true){
        nas_mac_process_cps_events(cps_read_fd);
    }
}


t_std_error nas_mac_send_cps_event_notification(void * data , int len){

    static int fd = nas_mac_get_write_cps_thread_fd();
    t_std_error rc;
    if(std_write(fd,data,len,true,&rc) != len){
        EV_LOGGING(L2MAC,ERR,"L2-FLUSH-NOT","Failed to send event header server");
        return rc;
    }

    return STD_ERR_OK;
}


t_std_error nas_mac_send_npu_event_notification(void * data, int len){
    static int fd = nas_mac_get_write_npu_thread_fd();
    t_std_error rc;
    if(std_write(fd,data,len,true,&rc) != len){
        EV_LOGGING(L2MAC,ERR,"L2-FLUSH-NOT","Failed to send event header server");
        return rc;
    }

    return STD_ERR_OK;
}
