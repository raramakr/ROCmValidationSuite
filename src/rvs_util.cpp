/********************************************************************************
 *
 * Copyright (c) 2018-2022 Advanced Micro Devices, Inc. All rights reserved.
 *
 * MIT LICENSE:
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is furnished to do
 * so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 *******************************************************************************/
#include "include/rvs_util.h"
#include "include/rvsloglp.h"
#include "include/gpu_util.h"
#include <vector>
#include <string>
#include <limits>
#include <regex>
#include <iomanip>
#include <fstream>
#include <algorithm>
#include "hip/hip_runtime.h"
#include "hip/hip_runtime_api.h"
#include "rocm_smi/rocm_smi.h"


/**
returns the current OS name
**/
std::string getOSName(){
  std::ifstream rel_file("/etc/os-release");
  if(!rel_file.good()){
    std::cout << "No /etc/os-release file, cant fetch details " << std::endl;
    return std::string{};
  }
  std::string line;
  while (std::getline(rel_file, line))
  {
                auto found = line.find("NAME") ;
    if (found!=std::string::npos){
      found = line.find('\"');
      auto endquote = line.find_last_of('\"');
      if(found == std::string::npos || endquote == std::string::npos)
        return std::string{};
      std::string os_name = line.substr(found+1, endquote-found-1 );
      return os_name;
    }
  }
}

/**
 * splits a std::string based on a given delimiter
 * @param str_val input std::string
 * @param delimiter tokens' delimiter
 * @return std::vector containing all tokens
 */

std::vector<std::string> str_split(const std::string& str_val,
                                   const std::string& delimiter) {
    std::vector<std::string> str_tokens;
    int prev_pos = 0, cur_pos = 0;
    do {
        cur_pos = str_val.find(delimiter, prev_pos);
        if (cur_pos == std::string::npos)
            cur_pos = str_val.length();
        std::string token = str_val.substr(prev_pos, cur_pos - prev_pos);
        if (!token.empty())
            str_tokens.push_back(token);
        prev_pos = cur_pos + delimiter.length();
    } while (cur_pos < str_val.length() && prev_pos < str_val.length());
    return str_tokens;
}


/**
 * checks if input std::string is a positive integer number
 * @param str_val the input std::string
 * @return true if std::string is a positive integer number, false otherwise
 */
bool is_positive_integer(const std::string& str_val) {
    return !str_val.empty()
            && std::find_if(str_val.begin(), str_val.end(),
                    [](char c) {return !std::isdigit(c);}) == str_val.end();
}

int rvs_util_parse(const std::string& buff, bool* pval) {
  if (buff.empty()) {  // method empty
    return 2;  // not found
  }

  if (buff == "true") {
    *pval = true;
    return 0;  // OK - true
  }

  if (buff == "false") {
    *pval = false;
    return 0;  // OK - false
  }

  return 1;  // syntax error
}

void *json_node_create(std::string module_name, std::string action_name,
                     int log_level){
        unsigned int sec;
        unsigned int usec;

        rvs::lp::get_ticks(&sec, &usec);
        void *json_node = rvs::lp::LogRecordCreate(module_name.c_str(),
                            action_name.c_str(), log_level, sec, usec, true);
        return json_node;
}

/**
 * summary: Fetches gpu id to index map for valid set of GPUs as per config.
 * Note: mcm_check is needed to output MCM specific messages while we iterate
 * through list, because iet power readings vary on this.
 * @out: map of gpu id to index in gpus_device_index and returns true if found 
 */
bool fetch_gpu_list(int hip_num_gpu_devices, map<int, uint16_t>& gpus_device_index,
                const std::vector<uint16_t>& property_device,
                const int& property_device_id, bool property_device_all, bool mcm_check){
    bool amd_gpus_found = false;
    bool mcm_die = false;
    bool amd_mcm_gpu_found = false;
    for (int i = 0; i < hip_num_gpu_devices; i++) {
        // get GPU device properties
        hipDeviceProp_t props;
        hipGetDeviceProperties(&props, i);

        // compute device location_id (needed in order to identify this device
        // in the gpus_id/gpus_device_id list
        unsigned int dev_location_id =
            ((((unsigned int) (props.pciBusID)) << 8) | ((unsigned int)(props.pciDeviceID)) << 3);
        uint16_t dev_domain = props.pciDomainID;
        uint16_t devId;
        uint16_t gpu_id;
        if (rvs::gpulist::domlocation2gpu(dev_domain, dev_location_id, &gpu_id)) {
            continue;
        }
        if (rvs::gpulist::gpu2device(gpu_id, &devId)){
            continue;
        }
        // filter by device id if needed
        if (property_device_id > 0 && property_device_id != devId)
          continue;

        // check if this GPU is part of the select ones as per config
        // (device = "all" or the gpu_id is in the device: <gpu id> list)
        bool cur_gpu_selected = false;

        if (property_device_all) {
            cur_gpu_selected = true;
        } else {
            // search for this gpu in the list
            // provided under the <device> property
            auto it_gpu_id = find(property_device.begin(),
                                  property_device.end(),
                                  gpu_id);

            if (it_gpu_id != property_device.end())
                cur_gpu_selected = true;
        }
	// if mcm check enabled, print message if device is MCM
	//  MCM is only for mi200, so check only if gfx90a
        if (mcm_check && (std::string{props.gcnArchName}.find("gfx90a") != std::string::npos) ){ 
	    std::stringstream msg_stream;
            mcm_die =  gpu_check_if_mcm_die(i);
	    if (mcm_die) {
                msg_stream.str("");
                msg_stream << "GPU ID : " << std::setw(5) << gpu_id << " - " << "Device : " << std::setw(5) << devId <<
                        " - " << "GPU is a die/chiplet in Multi-Chip Module (MCM) GPU";
                rvs::lp::Log(msg_stream.str(), rvs::logresults);

                amd_mcm_gpu_found = true;
            }
	}
	// excluding secondary die from test list, drops power reading substantially
	if (cur_gpu_selected) { // need not exclude secondary die, just log it out.
                    gpus_device_index.insert
                        (std::pair<int, uint16_t>(i, gpu_id));
                    amd_gpus_found = true;
	}
	mcm_die = false;
    }
    if (amd_mcm_gpu_found && mcm_check) {
        std::stringstream msg_stream;
        msg_stream.str("");
        msg_stream << "Note: The system has Multi-Chip Module (MCM) GPU/s." << "\n"
                   << "In MCM GPU, primary GPU die shows total socket (primary + secondary) power information." << "\n"
                   << "Secondary GPU die does not have any power information associated with it independently."<< "\n"
                   << "So, expect power reading from Secondary GPU die as 0."<< "\n";
        rvs::lp::Log(msg_stream.str(), rvs::logresults);
     }
    return amd_gpus_found;
}

void firmware_version_get(int idx, rsmi_fw_block_t blk, uint64_t* val){
  auto ret = rsmi_dev_firmware_version_get(idx, blk, val);
  if (ret != RSMI_STATUS_SUCCESS)
	 *val = std::numeric_limits<uint64_t>::max(); 
}

void fill_firmware_versions(int idx, fm_ver& fmver){
    firmware_version_get(idx, RSMI_FW_BLOCK_ASD      ,&fmver.asd);
    firmware_version_get(idx, RSMI_FW_BLOCK_CE       ,&fmver.ce);
    firmware_version_get(idx, RSMI_FW_BLOCK_DMCU     ,&fmver.dmcu);
    firmware_version_get(idx, RSMI_FW_BLOCK_MC       ,&fmver.mc);
    firmware_version_get(idx, RSMI_FW_BLOCK_ME       ,&fmver.me);
    firmware_version_get(idx, RSMI_FW_BLOCK_MEC      ,&fmver.mec);
    firmware_version_get(idx, RSMI_FW_BLOCK_MEC2     ,&fmver.mec2);
    firmware_version_get(idx, RSMI_FW_BLOCK_PFP      ,&fmver.pfp);
    firmware_version_get(idx, RSMI_FW_BLOCK_RLC      ,&fmver.rlc);
    firmware_version_get(idx, RSMI_FW_BLOCK_RLC_SRLC ,&fmver.rlc_srlc);
    firmware_version_get(idx, RSMI_FW_BLOCK_RLC_SRLG ,&fmver.rlc_srlg);
    firmware_version_get(idx, RSMI_FW_BLOCK_RLC_SRLS ,&fmver.rlc_srls);
    firmware_version_get(idx, RSMI_FW_BLOCK_SDMA     ,&fmver.sdma);
    firmware_version_get(idx, RSMI_FW_BLOCK_SDMA2    ,&fmver.sdma2);
    firmware_version_get(idx, RSMI_FW_BLOCK_SMC      ,&fmver.smc);
    firmware_version_get(idx, RSMI_FW_BLOCK_SOS      ,&fmver.sos);
    firmware_version_get(idx, RSMI_FW_BLOCK_TA_RAS   ,&fmver.ta_ras);
    firmware_version_get(idx, RSMI_FW_BLOCK_TA_XGMI  ,&fmver.ta_xgmi);
    firmware_version_get(idx, RSMI_FW_BLOCK_UVD   ,&fmver.uvd);
    firmware_version_get(idx, RSMI_FW_BLOCK_VCE   ,&fmver.vce);
    firmware_version_get(idx, RSMI_FW_BLOCK_VCN   ,&fmver.vcn);
}

std::string cleanprint(uint64_t val){
  std::string tmp{"NA"};
  if ( val != std::numeric_limits<uint64_t>::max())
	  tmp = std::to_string(val);
  return tmp;
}
void print_fmversions(const fm_ver& fmver){
  std::cout << "    BLOCK_ASD     :  " << cleanprint(fmver.asd) << "\n"
	    << "    BLOCK_CE      :  " << cleanprint(fmver.ce)  << "\n"
	    << "    BLOCK_DMCU    :  " << cleanprint(fmver.dmcu)  << "\n"
	    << "    BLOCK_MC      :  " << cleanprint(fmver.mc)  << "\n"
	    << "    BLOCK_ME      :  " << cleanprint(fmver.me)  << "\n"
	    << "    BLOCK_MEC     :  " << cleanprint(fmver.mec)  << "\n"
	    << "    BLOCK_MEC2    :  " << cleanprint(fmver.mec2)  << "\n"
	    << "    BLOCK_PFP     :  " << cleanprint(fmver.pfp)  << "\n"
	    << "    BLOCK_RLC     :  " << cleanprint(fmver.rlc)  << "\n"
	    << "    BLOCK_RLC_SRLC:  " << cleanprint(fmver.rlc_srlc)  << "\n"
	    << "    BLOCK_RLC_SRLG:  " << cleanprint(fmver.rlc_srlg)  << "\n"
	    << "    BLOCK_RLC_SRLS:  " << cleanprint(fmver.rlc_srls)  << "\n"
	    << "    BLOCK_SDMA    :  " << cleanprint(fmver.sdma)  << "\n"
	    << "    BLOCK_SDMA2   :  " << cleanprint(fmver.sdma2)  << "\n"
	    << "    BLOCK_SMC     :  " << cleanprint(fmver.smc)  << "\n"
	    << "    BLOCK_SOS     :  " << cleanprint(fmver.sos)  << "\n"
	    << "    BLOCK_TA_RAS  :  " << cleanprint(fmver.ta_ras)  << "\n"
	    << "    BLOCK_TA_XGMI :  " << cleanprint(fmver.ta_xgmi)  << "\n"
	    << "    BLOCK_UVD     :  " << cleanprint(fmver.uvd)  << "\n"
	    << "    BLOCK_VCE     :  " << cleanprint(fmver.vce)  << "\n"
	    << "    BLOCK_VCN     :  " << cleanprint(fmver.vcn)  << std::endl;
}

int display_gpu_info (void) {

  struct device_info {
    std::string bus;
    std::string name;
    int32_t node_id;
    int32_t gpu_id;
    int32_t device_id;
    char vbios_ver[1024];
    fm_ver fmver;
  };
  char buff[1024];
  int hip_num_gpu_devices;
  std::string errmsg = " No supported GPUs available.";
  std::vector<device_info> gpu_info_list;

  hipGetDeviceCount(&hip_num_gpu_devices);
  if( hip_num_gpu_devices == 0){
    std::cout << std::endl << errmsg << std::endl;
    return 0;
  }
  rsmi_init(0);
  for (int i = 0; i < hip_num_gpu_devices; i++) {
    hipDeviceProp_t props;
    hipGetDeviceProperties(&props, i);
    
    // compute device location_id (needed in order to identify this device
    // in the gpus_id/gpus_device_id list
    unsigned int dev_location_id =
        ((((unsigned int) (props.pciBusID)) << 8) | ((unsigned int)(props.pciDeviceID)) << 3);
    uint16_t dev_domain = props.pciDomainID;  
    uint16_t node_id;
    if (rvs::gpulist::domlocation2node(dev_domain, dev_location_id, &node_id)) {
      continue;
    }
    uint16_t gpu_id;
    if (rvs::gpulist::domlocation2gpu(dev_domain, dev_location_id, &gpu_id)) {
      continue;
    }
    uint16_t dev_id;
    if (rvs::gpulist::gpu2device(gpu_id, &dev_id)){
      continue;
    }
    snprintf(buff, sizeof(buff), "%04d:%02X:%02X.%d",props.pciDomainID, props.pciBusID, props.pciDeviceID, 0);
    device_info info;
    info.bus       = buff;
    info.name      = props.name;
    info.node_id   = node_id;
    info.gpu_id    = gpu_id;
    info.device_id = dev_id;
    auto ret = rsmi_dev_vbios_version_get(i, info.vbios_ver, 1024);// check returns TODO
    info.fmver = {};
    fill_firmware_versions(i, info.fmver);
    gpu_info_list.push_back(info);

  }
  std::string rcm{ROCM_PATH};
  if (rcm.find('/') != std::string::npos){
	  rcm = rcm.substr(rcm.find_last_of('/')+1);
  }
  std::cout  << rcm << " Running on " << getOSName() << std::endl;
  std::sort(gpu_info_list.begin(), gpu_info_list.end(),
           [](const struct device_info& a, const struct device_info& b) {
             return a.node_id < b.node_id; });
  if (!gpu_info_list.empty()) {
    std::cout << "Supported GPUs available:\n";
    for (const auto& info : gpu_info_list) {
      std::cout << info.bus  << " - GPU[" << std::setw(2) << info.node_id
      << " - " << std::setw(5) << info.gpu_id << "] " << info.name
      << " (Device " << info.device_id << ")" << "\n"
      <<"  VBIOS Version:" << std::string(info.vbios_ver) << "\n"
      << "  FirmWare Versions are: \n";
      print_fmversions(info.fmver);
    }
  } else {
      std::cout << std::endl << errmsg << std::endl;
  }
  rsmi_shut_down();
  return 0;
}
