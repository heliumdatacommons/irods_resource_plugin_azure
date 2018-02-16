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

#define AZURE_ACCOUNT "AZURE_ACCOUNT"
#define AZURE_ACCOUNT_KEY "AZURE_ACCOUNT_KEY"
#define AZURE_HTTPS_STRING "DefaultEndpointsProtocol=https"
#define AZURE_ACCOUNT_FILE "AZURE_ACCOUNT_FILE"
#define MAX_ACCOUNT_SIZE  256
#define MAX_ACCOUNT_KEY_SIZE  256
#endif
