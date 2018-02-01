#ifndef LIB_AZURE_HPP
#define LIB_AZURE_HPP
/**
 * @file
 * @version 1.0
 *
 * @section LICENSE
 *
 * This software is open source.
 *
 * Renaissance Computing Institute,
 * (A Joint Institute between the University of North Carolina at Chapel Hill,
 * North Carolina State University, and Duke University)
 * http://www.renci.org
 *
 * For questions, comments please contact software@renci.org
 *
 * @section DESCRIPTION
 *
 * This file contains the definitions for the c code to exercise the Microsoft Azure Interface
 */

#include "irods_resource_plugin.hpp"
// azure includes
#include <was/storage_account.h>
#include <was/blob.h>
#include <cpprest/filestream.h>
#include <cpprest/containerstream.h>
#include "libazure.hpp"

#define AZURE_ACCOUNT "AZURE_ACCOUNT"
#define AZURE_ACCOUNT_KEY "AZURE_ACCOUNT_KEY"
#define AZURE_HTTPS_STRING "DefaultEndpointsProtocol=https"
#define AZURE_ACCOUNT_FILE "AZURE_ACCOUNT_FILE"
#define MAX_ACCOUNT_SIZE  256
#define MAX_ACCOUNT_KEY_SIZE  256

extern "C" {

static irods::error putTheFile(
    utility::string_t       connection,
    const char*             complete_file_path,
    const std::string       file_name,
    const char*             prev_physical_path,
    const std::string       container,
    irods::plugin_context& _ctx);

static bool getTheFile(
    std::string       container,
    std::string       file_name,
    utility::string_t connection,
    const char       *destination,
    int               mode);

static bool
getTheFileStatus (const std::string container, const std::string file,  const utility::string_t connection);

static bool deleteTheFile (const std::string container, const std::string file,  const utility::string_t connection);


}; // extern "C" 
#endif
