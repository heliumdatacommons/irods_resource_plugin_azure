


// =-=-=-=-=-=-=-
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <getopt.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/stat.h>

// =-=-=-=-=-=-=-
// irods includes
#include "rsRegReplica.hpp"
#include "rsModDataObjMeta.hpp"
#include "irods_server_properties.hpp"
#include "irods_hasher_factory.hpp"


// =-=-=-=-=-=-=-
// azure includes
#include "libazure.hpp"

// =-=-=-=-=-=-=-
// irods includes
#include "irods_resource_plugin.hpp"
#include "irods_hasher_factory.hpp"
#include "irods_file_object.hpp"
#include "irods_physical_object.hpp"
#include "irods_collection_object.hpp"
#include "irods_string_tokenize.hpp"
#include "irods_hierarchy_parser.hpp"
#include "irods_resource_redirect.hpp"
#include "irods_stacktrace.hpp"
#include "irods_virtual_path.hpp"
#include "irods_kvp_string_parser.hpp"
#include "MD5Strategy.hpp"
#include "rsRegDataObj.hpp"

// =-=-=-=-=-=-=-
// stl includes
#include <iostream>
#include <sstream>
#include <vector>
#include <string>
#include "boost/lexical_cast.hpp"
#include "boost/filesystem.hpp"

// =-=-=-=-=-=-=-
// azure includes
#include <was/storage_account.h>
#include <was/blob.h>
#include <cpprest/filestream.h>  
#include <cpprest/containerstream.h> 

static const std::string REPL_POLICY_KEY( "repl_policy" );

irods::error readAzureAccountInfo (
    const std::string& _filename,
    std::string& _rtn_account,
    std::string& _rtn_account_key)
{
    irods::error result = SUCCESS();
    irods::error ret;
    FILE *fptr;
    char inbuf[MAX_ACCOUNT_SIZE];
    int lineLen, bytesCopied;
    int linecnt = 0;
    char account[MAX_ACCOUNT_SIZE];
    char account_key[MAX_ACCOUNT_KEY_SIZE];

    fptr = fopen (_filename.c_str(), "r");

    if ((result = ASSERT_ERROR(fptr != NULL, SYS_CONFIG_FILE_ERR, "Failed to open Azure auth file: \"%s\", errno = \"%s\".",
                               _filename.c_str(), strerror(errno))).ok()) {
        while ((lineLen = getLine (fptr, inbuf, MAX_NAME_LEN)) > 0) {
            char *inPtr = inbuf;
            if (linecnt == 0) {
                while ((bytesCopied = getStrInBuf (&inPtr, account, &lineLen, MAX_ACCOUNT_SIZE)) > 0) {
                    linecnt ++;
                    break;
                }
            } else if (linecnt == 1) {
                while ((bytesCopied = getStrInBuf (&inPtr, account_key, &lineLen, MAX_ACCOUNT_KEY_SIZE)) > 0) {
                    linecnt ++;
                    break;
                }
            }
        }
        if ((result = ASSERT_ERROR(linecnt == 2, SYS_CONFIG_FILE_ERR, "Read %d lines in the auth file. Expected 2.",
                                   linecnt)).ok())  {
            _rtn_account = account;
            _rtn_account_key = account_key;
        }
        return result;
    }

    result = ERROR( SYS_CONFIG_FILE_ERR, "Unknown error in authorization file." );
    return result;
}

   // =-=-=-=-=-=-=-
   /// @brief Checks the basic operation parameters and updates the physical path in the file object
   irods::error azureCheckParams(irods::plugin_context& _ctx ) {

       irods::error ret;

       // =-=-=-=-=-=-=-
       // check incoming parameters
       // =-=-=-=-=-=-=-
       // verify that the resc context is valid
       ret = _ctx.valid();
       return ( ASSERT_PASS(ret, "azureCheckParams - resource context is invalid"));

   } // azureCheckParams

   /**
    * @brief This function hashes the file part of a physical path name.  We need to do this
    * because in microsoft azure, directory names (known as containers) have to be all lower case
    * strings between 3 and 64 characters.  Note that if the first character is not a '/' we
    * are assuming the path has already been hashed, so we can return everthing up to and not 
    * including the '/'. Note that we are assuming we will never store anything in a sub-container.
    *
    * @return Either the hashed version of the file path not including the file name or the 
    * existing hash (again not including the file name).
    */
   std::string getHashedContainerName(std::string physical_path) {
      irods::error prop_ret;
      std::string hashed_name;

      if ( physical_path.empty() ) {
         THROW (SYS_INVALID_INPUT_PARAM, "physical_path is empty");
      }

      size_t first_slash = physical_path.find_first_of('/');

      // Now make sure there is at least one '/' in the path
      if (first_slash == std::string::npos) {
         THROW (SYS_INVALID_INPUT_PARAM, boost::format("physical_path is invalid: %s") % physical_path.c_str());
      }

      // Next, we check to see if the the first character is a '/'.  If it's not, than this path should be
      // of the form "checksum/filename" and we want to return the checksum part.
      if (first_slash != 0) {
         return (physical_path.substr(0, first_slash));
      }

      // if we are here, we have a "normal" slash de-limited file name. It may be slightly overkill, but
      // we use the boost filesystem path to get the parent path
      boost::filesystem::path boost_physical = boost::filesystem::path(physical_path);
      boost::filesystem::path path_to_be_hashed = boost_physical.parent_path();
      std::string string_path = path_to_be_hashed.filename().string();

      // Now that we have the path, let's hash it
      irods::Hasher hasher;
      irods::getHasher( irods::MD5_NAME, hasher );
      hasher.update(std::string(string_path, string_path.length()));
      
      // if anything fails, we won't be here...
      hasher.digest(hashed_name);
      return hashed_name;
   }


   /**
    * @brief This function returns the file part of a physical path name.  This handles both the
    * case of a true physical path name and the case of a path that consists of checksum/filename
    *
    * @return theFileName
    */
   std::string getFileName(std::string physical_path) {
      irods::error prop_ret;
      std::string hashed_name;

      if ( physical_path.empty() ) {
         THROW (SYS_INVALID_INPUT_PARAM, "physical_path is empty");
      }

      size_t first_slash = physical_path.find_first_of('/');

      // Now make sure there is at least one '/' in the path
      if (first_slash == std::string::npos) {
         THROW (SYS_INVALID_INPUT_PARAM, boost::format("physical_path is invalid: %s") % physical_path.c_str());
      }

      // Next, we check to see if the the first character is a '/'.  If it's not, than this path should be
      // of the form "checksum/filename" and we want to return the checksum part.
      if (first_slash != 0) {
         return (physical_path.substr(first_slash + 1));
      }

      // if we are here, we have a "normal" slash de-limited file name. It may be slightly overkill, but
      // we use the boost filesystem path to get the parent path
      boost::filesystem::path boost_physical = boost::filesystem::path(physical_path);
      boost::filesystem::path file_name = boost_physical.filename();
      return(file_name.filename().string());
   }


   /**
    * @brief This function creates the AZURE connection string needed in many places in this file.
    *
    * @return The required connection string. Empty on failure
    */
   utility::string_t createConnectionString(irods::plugin_context& _ctx) {
      irods::error prop_ret;
      std::string account_file;
      std::string my_account;
      std::string my_key;
      std::string my_azure = AZURE_HTTPS_STRING;
      const char *azure_account = nullptr;
      const char *azure_key = nullptr;
      utility::string_t azure_connection;

      // check incoming parameters
      irods::error ret = azureCheckParams( _ctx );
      if ( !ret.ok() ) {
         THROW (SYS_INVALID_INPUT_PARAM, boost::format("Context failed validation check"));
      }

      irods::plugin_property_map& prop_map = _ctx.prop_map();
      prop_ret = prop_map.get< std::string >( AZURE_ACCOUNT_FILE, account_file );

      if ( !prop_ret.ok() ) {
         THROW (SYS_INVALID_INPUT_PARAM, boost::format("Failed to get property: %s") % AZURE_ACCOUNT_FILE);
      }
      ret = readAzureAccountInfo(account_file, my_account, my_key);
      if ( !ret.ok() ) {
         THROW (SYS_INVALID_INPUT_PARAM, boost::format("Failed to read the account file %s") % account_file);
      }
      azure_account = my_account.c_str();
      azure_key = my_key.c_str();

//      prop_ret = prop_map.get< std::string >( AZURE_ACCOUNT, my_account );

//      if ( !prop_ret.ok() ) {
 //        THROW (SYS_INVALID_INPUT_PARAM, boost::format("Failed to get property: %s") % AZURE_ACCOUNT);
 //     }
//      azure_account = my_account.c_str();

//      prop_ret = prop_map.get< std::string >( AZURE_ACCOUNT_KEY, my_key );
//      if ( !prop_ret.ok() ) {
//         THROW (SYS_INVALID_INPUT_PARAM, boost::format("Failed to get property: %s") % AZURE_ACCOUNT_KEY);
//      }
//      azure_key = my_key.c_str();

      // Construct the azure connection string.
      azure_connection = my_azure + ";AccountName=" + my_account + ";AccountKey=" + my_key;

      // if anything fails, we won't be here...
      return azure_connection;
   }


/*
 * This function is called to put a file into Azure storage
 *
 */

static irods::error putNewFile( const       std::string container, 
                               const char*  complete_file_path, 
                               const        std::string  file_name, 
                               const        utility::string_t connection) {

   irods::error result = SUCCESS();
   
   try { 
      // Retrieve storage account from connection string.
     rodsLog(LOG_DEBUG6, "putNewFile: container %s complete_file_path %s, file_name %s", 
             container.c_str(), complete_file_path, file_name.c_str());
      azure::storage::cloud_storage_account account = 
         azure::storage::cloud_storage_account::parse(connection);

      // Create the blob client.
      azure::storage::cloud_blob_client client = account.create_cloud_blob_client();

      // Get the container reference
      azure::storage::cloud_blob_container azure_container = 
         client.get_container_reference(U(container));

      // create the container if it's not there already
      azure_container.create_if_not_exists();

      // Retrieve reference to a blob 
      azure::storage::cloud_block_blob blob = azure_container.get_block_blob_reference(U(file_name));

      // Create or overwrite the blob with contents from the local file
      concurrency::streams::istream input_stream = 
         concurrency::streams::file_stream<uint8_t>::open_istream(U(complete_file_path)).get();
      blob.upload_from_stream(input_stream);
      input_stream.close().wait();

   } catch (const std::exception& e) {
      // Note that in theory, it might be a good idea to try to delete the  
      // file from Azure at this point, in case of a partial write.  We aren't
      // going to do this because A) Hopefully the Azure call will roll back any partial
      // write on failure B) From the point of view of iRODS the file isn't there and C)
      // the default semantic of Azure is to overwrite existing files the next time a file
      // of the same name is written.
      rodsLog (LOG_ERROR, "Could not put file in AZURE - [%s]", e.what());
      result = ERROR( UNIV_MSS_SYNCTOARCH_ERR, e.what());
   }

   return result;
}


/** 
 * @brief This function is the high level function that adds a data file
 *        to the AZURE storage.  It uses the microsoft libazurestorage to do this.
 *
 *  This function checks to see if the file we are putting is already on azure. If it
 *  is, then we delete the existing file and add the new one.
 *
 * @param complete_file_path The complete file path for the file
 * @param file_name The file name component
 * @param prev_physical_path The previos physical path
 * @param container The Azure container
 * @param _ctx The irods context
 * @return res.  The irods::error results
 */
static irods::error putTheFile(
    utility::string_t      connection,
    const char*            complete_file_path, 
    const std::string      file_name,
    const char*            prev_physical_path,
    const std::string      container,
    irods::plugin_context& _ctx) {

    // NOTE: At some point we will need to need to look at the "force flag" to do some error checking.
    // For now, we are just going to see if the file is already there. If it is, we will delete it
    // and re-add the new version.

    // =-=-=-=-=-=-=-
    // stat the file, if it exists on the system we need to check 
    // the size.  If it is non-zero, register first to get OID and then overwrite.
    // If it is zero then just put the file. 
    bool status = 1;
    irods::error result = SUCCESS();
    std::string prev_physical_str( prev_physical_path );
    rodsLog(LOG_DEBUG6, "Putting the file: complete_file_path %s, file_name %s", 
            complete_file_path, file_name.c_str());

    // only query if the file path does not start with a "/".  Files already on azure will not.
    if (0 != prev_physical_str.find( "/")) {
        status = getTheFileStatus( 
                     container, 
                     file_name,
                     connection);
    }

    // non-zero status means file was there.  We technically don't need this since azure seems
    // to overwrite by default.  Left here because we may want (and be able to) change that behavior....
    if( status ) {
       // lets delete the file
       rodsLog(LOG_DEBUG6, 
               "Calling deleteTheFile in putTheFile: container %s file_name %s" , 
                container.c_str(), file_name.c_str());
       status = deleteTheFile(container, file_name, connection); 
       rodsLog(LOG_DEBUG6, "Result of deleteTheFile is %d", status);
       if (status == false) {
          rodsLog(LOG_DEBUG6, "Erroring out of putTheFile after the delete");
          result = ERROR( UNIV_MSS_SYNCTOARCH_ERR,
                          std::string("azureSyncToArchPlugin - error in deleteTheFile" ));
          return result;
       }
    }

    rodsLog(LOG_DEBUG6, 
           "Calling putNewFile in putTheFile: container %s file_name %s complete_file_path %s" , 
            container.c_str(), file_name.c_str(), complete_file_path);
    result = putNewFile( container, complete_file_path,  file_name, connection);

    rodsLog(LOG_DEBUG, "finished writing file - %s", complete_file_path);

    return result;
}

/** 
 * @brief This function is the high level function that retrieves a data file
 *        from the DDN storage using the Azure interface.
 *
 *  This function uses the Azure API to GET the specified file
 *  from Azure and put it in the cache.
 *
 * @param container The Azure container
 * @param file_name The file name component
 * @param resource A character pointer to the resource for this request.
 * @param connection The Azure connection string
 * @param destination A character pointer to the destination for this request.
 *        This is a file.
 * @param mode The file mode to be used when creating the file.  This comes
 *        from the users original file and is stored in the icat.
 * @return res.  The return code from curl_easy_perform.
 */

static bool getTheFile(
    std::string       container,
    std::string       file_name,
    utility::string_t connection,
    const char       *destination, 
    int               mode) {

    const utility::string_t destination_string = destination;

    // library
    const int destFd = open(destination, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (destFd < 0) {
        return(UNIX_FILE_OPEN_ERR - errno);
    } else {
       close(destFd);
       try {
          // Retrieve storage account from connection string.
          azure::storage::cloud_storage_account account = 
             azure::storage::cloud_storage_account::parse(connection);

          // Create the blob client.
          azure::storage::cloud_blob_client client = account.create_cloud_blob_client();

          // Retrieve a reference to a previously created container.
          azure::storage::cloud_blob_container azure_container = 
             client.get_container_reference(U(container));

          // Retrieve reference to a blob named "my-blob-2".
          azure::storage::cloud_block_blob text_blob = 
             azure_container.get_block_blob_reference(U(file_name));

          // Download the contents of a blog to the file
          // Not sure if this will preserve the mode we set above.
          text_blob.download_to_file(destination_string);
       } catch  (const std::exception& e) {
          rodsLog (LOG_ERROR, "Could not check file existence in AZURE - [%s]", e.what());
          unlink(destination);
          return (false);
       }
    } 

    return true;

}

/** 
 * @brief This function is the low level function that retrieves the 
 *        status of a data file from the azure storage using the libazurestorage library.
 *
 *  This function checks to see if the specified file exists in azure. If so, it returns true, 
 *  otherwise false.  It may be possible to to get more info (like the size of the file) but
 *  that will be for later.
 *
 * @param container The container for the file (normally the checksum of a directory path)
 * @param file The file in which we are interested.
 * @param connection The azure connection string.
 * @return res.  Whether or not the file exists.
 */

static bool 
getTheFileStatus (const std::string container, const std::string file,  const utility::string_t connection) {

   try {
      // Retrieve storage account from connection string.
      azure::storage::cloud_storage_account storage_account = 
         azure::storage::cloud_storage_account::parse(connection);

      // Create the blob client.
      azure::storage::cloud_blob_client blob_client = 
         storage_account.create_cloud_blob_client();

      // Get the container
      azure::storage::cloud_blob_container azure_container = blob_client.get_container_reference(U(container));

      // Now get the blob
      azure::storage::cloud_block_blob the_blob = azure_container.get_block_blob_reference(U(file));
      
      // Return whether or not this blob exists.
      return (the_blob.exists());
   }
   catch (const std::exception& e) {
      rodsLog (LOG_ERROR, "Could not check file existence in AZURE - [%s]", e.what());
      return (false);
   }

}

/** 
 * @brief This function is the low level function that retrieves the 
 *        length of a data file from the azure storage using the libazurestorage library.
 *
 *  This function checks to see if the specified file exists in azure. If so, it continues 
 *  on to return the length of the file, otherwise it returns -1 (0 length may be possible)
 *
 * @param container The container for the file (normally the checksum of a directory path)
 * @param file The file in which we are interested.
 * @param connection The azure connection string.
 * param  length
 * @return true on success, false on failure
 */

static bool 
getTheFileLength (const std::string        container, 
                  const std::string        file,  
                  const utility::string_t  connection, 
                  rodsLong_t              *length) {

   bool result = true;
   utility::size64_t azure_length;

   try {
      rodsLog(LOG_DEBUG6, "Getting the length of %s", file.c_str());
      // Retrieve storage account from connection string.
      azure::storage::cloud_storage_account storage_account = 
         azure::storage::cloud_storage_account::parse(connection);

      // Create the blob client.
      azure::storage::cloud_blob_client blob_client = 
         storage_account.create_cloud_blob_client();

      // Get the container
      azure::storage::cloud_blob_container azure_container = blob_client.get_container_reference(U(container));

      // Now get the blob
      azure::storage::cloud_block_blob the_blob = azure_container.get_block_blob_reference(U(file));
      
      // if the file existe. we get it's length
      if (the_blob.exists()) {
         azure::storage::cloud_blob_properties &props = the_blob.properties();
         azure_length = props.size();
         *length = static_cast<rodsLong_t>(azure_length);
          rodsLog(LOG_DEBUG6, "Setting the length of %s to %ld", file.c_str(), *length);
      } else {
        // The file doesn't exist. Signal this to the caller.
        *length = -1;
     }
   } catch (const std::exception& e) {
      rodsLog (LOG_ERROR, "Could not check file existence in AZURE - [%s]", e.what());
      return (false);
   }
   
   return result;
}

/** 
 * @brief This function is the high level function that deletes a data file
 *        from azure storage using the libazure interface. 
 *
 * @param container The container for the file (normally the checksum of a directory path)
 * @param file The file in which we are interested.
 * @param connection The azure connection string.
 * @return res.  Whether or not the file was successfully deleted. Note that if the file was not
 *               there to be deleted, the function returns true.  This preserves the semantic
 *               that after this code is executed, the file is not on the system.
 */
static bool deleteTheFile (const std::string container, 
                           const std::string file,  
                           const utility::string_t connection) {

   bool result = true;

   const utility::string_t utilConnection = connection;
   try {
      // Retrieve storage account from connection string.
      rodsLog(LOG_DEBUG6, "In deleteTheFile: Retrieving the storage account from %s", utilConnection.c_str());
      azure::storage::cloud_storage_account storage_account = 
         azure::storage::cloud_storage_account::parse(utilConnection);

      rodsLog(LOG_DEBUG6, "Trying to create the client");
      // Create the blob client.
      azure::storage::cloud_blob_client blob_client = 
         storage_account.create_cloud_blob_client();
      rodsLog(LOG_DEBUG6, "Created the client");

      // Get the container
      rodsLog(LOG_DEBUG6, "Trying to get the container for %s", container.c_str());
      azure::storage::cloud_blob_container azure_container = blob_client.get_container_reference(U(container));
      rodsLog(LOG_DEBUG6, "Got the container");

      // Now get the blob
      azure::storage::cloud_block_blob the_blob = azure_container.get_block_blob_reference(U(file));
      
      if (the_blob.exists()) {
         // Try to delete the blob.
         the_blob.delete_blob();
         return (result);
      } else {
         // you tried to delete it, and it's not there. No reason to call that a failure.
         return (result);
      }
   }
   catch (const std::exception& e) {
      rodsLog (LOG_ERROR, "Could not delete blob in AZURE - [%s]", e.what());
      return (false);
   }
}
    
    // =-=-=-=-=-=-=-
    // interface for file registration
    irods::error azureRegisteredPlugin( irods::plugin_context& _ctx) {

        return ERROR( SYS_NOT_SUPPORTED, "azureRegisteredPlugin" );
    } // azureRegisteredPlugin

    // =-=-=-=-=-=-=-
    // interface for file unregistration
    irods::error azureUnregisteredPlugin( irods::plugin_context& _ctx) {

        return ERROR( SYS_NOT_SUPPORTED, "azureUnregisteredPlugin" );
    } // azureUnregisteredPlugin

    // =-=-=-=-=-=-=-
    // interface for file modification
    irods::error azureModifiedPlugin( irods::plugin_context& _ctx) {

        return ERROR( SYS_NOT_SUPPORTED, "azureModifiedPlugin" );
    } // azureModifiedPlugin
    
    // =-=-=-=-=-=-=-
    // interface for POSIX create
    irods::error azureFileCreatePlugin( irods::plugin_context& _ctx) {

        return ERROR( SYS_NOT_SUPPORTED, "azureFileCreatePlugin" );
    } // azureFileCreatePlugin

    // =-=-=-=-=-=-=-
    // interface for POSIX Open
    irods::error azureFileOpenPlugin( irods::plugin_context& _ctx) {

        return ERROR( SYS_NOT_SUPPORTED, "azureFileOpenPlugin" );
    } // azureFileOpenPlugin

    // =-=-=-=-=-=-=-
    // interface for POSIX Read
    irods::error azureFileReadPlugin( irods::plugin_context& _ctx,
                                      void*               _buf, 
                                      int                 _len ) {
                                      
        return ERROR( SYS_NOT_SUPPORTED, "azureFileReadPlugin" );

    } // azureFileReadPlugin

    // =-=-=-=-=-=-=-
    // interface for POSIX Write
    irods::error azureFileWritePlugin( irods::plugin_context& _ctx,
                                       void*               _buf, 
                                       int                 _len ) {
        return ERROR( SYS_NOT_SUPPORTED, "azureFileWritePlugin" );

    } // azureFileWritePlugin

    // =-=-=-=-=-=-=-
    // interface for POSIX Close
    irods::error azureFileClosePlugin(  irods::plugin_context& _ctx ) {

        return ERROR( SYS_NOT_SUPPORTED, "azureFileClosePlugin" );
        
    } // azureFileClosePlugin

    // =-=-=-=-=-=-=-
    // interface for POSIX Unlink
    irods::error azureFileUnlinkPlugin( irods::plugin_context& _ctx ) {
        bool status;
        std::string storage_connection;

        irods::error prop_ret;
        std::string my_account;
        std::string my_key;
        std::ostringstream out_stream;
        irods::error result = SUCCESS();

        // =-=-=-=-=-=-=-
        // check incoming parameters
        try {
           const utility::string_t connection = createConnectionString(_ctx);
      
           // =-=-=-=-=-=-=-
           // get ref to data object
           irods::file_object_ptr file_obj =
               boost::dynamic_pointer_cast< irods::file_object >( _ctx.fco() );

           // Note that this function can handle the case where the physical path has already been
           // hashed
           std::string container = getHashedContainerName(file_obj->physical_path());
           std::string fileName = getFileName(file_obj->physical_path());
       
           rodsLog(LOG_DEBUG6, 
                   "Calling deleteTheFile in azureFileUnlinkPlugin: container %s fileName %s" , 
                   container.c_str(), fileName.c_str());
           status = deleteTheFile(container, fileName, connection);

           // error handling
           if( status != true ) {
               result =  ERROR( UNLINK_FAILED, "azureFileUnlinkPlugin - error in deleteTheFile");
           }
        } catch ( const irods::exception& e) {
           irods::log( irods::error(e) );
           result = ERROR( SYS_INVALID_INPUT_PARAM, e.what());
        }

        return result;
    } // azureFileUnlinkPlugin

    // =-=-=-=-=-=-=-
    // interface for POSIX Stat
    irods::error azureFileStatPlugin(
        irods::plugin_context& _ctx,
        struct stat*           _statbuf ) { 
        rodsLong_t len;
        int status = 0;
        irods::error result = SUCCESS();

        // =-=-=-=-=-=-=-
        // check incoming parameters
        try {
           // Get the connection
           const utility::string_t connection = createConnectionString(_ctx);
           irods::file_object_ptr file_obj = boost::dynamic_pointer_cast< irods::file_object >( _ctx.fco() );

           // the hashed container name is the hash of the physical path not including the file name.
           std::string container = getHashedContainerName(file_obj->physical_path());
           std::string file_name = getFileName(file_obj->physical_path());
           rodsLog(LOG_DEBUG6, "Physical path is %s", file_obj->physical_path().c_str());
      

           // Check the file status
           status = getTheFileLength(container, file_name, connection, &len);
           rodsLog(LOG_DEBUG6, "After getTheFileLength: container %s file_name %s len %ld status %d",
                                container.c_str(), file_name.c_str(), len, status);
      
           // returns non-zero on error.
           if (status) {
                 // Fill in the rest of the struct.  Note that this code is carried over
                 // from the original code.
                 if (len >= 0 ) {
                    rodsLog(LOG_DEBUG6, "Len > 0: %ld", len);
                    _statbuf->st_mode = S_IFREG;
                    _statbuf->st_nlink = 1;
                    _statbuf->st_uid = getuid ();
                    _statbuf->st_gid = getgid ();
                    _statbuf->st_atime = _statbuf->st_mtime = _statbuf->st_ctime = time(0);
                    _statbuf->st_size = len;
                 } else {
                    // the file was not found
                    rodsLog(LOG_DEBUG6, "File not found case: %d", status);
                    result =  ERROR( status, "azureFileStatPlugin - file not found");
                 }
              } else {
                rodsLog(LOG_DEBUG6, "azureFileStatPlugin - error in getTheFileStatus");
                result =  ERROR( status, "azureFileStatPlugin - error in getTheFileStatus");
              } 
           } catch ( const irods::exception& e) {
              irods::log( irods::error(e) );
              rodsLog(LOG_DEBUG6, "Exception in azureFileStatPlugin: %s", e.what());
              result = ERROR( SYS_INVALID_INPUT_PARAM, e.what());
           }
        return result;

    } // azureFileStatPlugin

    // =-=-=-=-=-=-=-
    // interface for POSIX lseek
    irods::error azureFileLseekPlugin(  irods::plugin_context& _ctx, 
                                       size_t              _offset, 
                                       int                 _whence ) {

        return ERROR( SYS_NOT_SUPPORTED, "azureFileLseekPlugin" );
                                       
    } // azureFileLseekPlugin

    // =-=-=-=-=-=-=-
    // interface for POSIX fsync
    irods::error azureFileFsyncPlugin(  irods::plugin_context& _ctx ) {

        return ERROR( SYS_NOT_SUPPORTED, "azureFileFsyncPlugin" );

    } // azureFileFsyncPlugin

    // =-=-=-=-=-=-=-
    // interface for POSIX mkdir
    irods::error azureFileMkdirPlugin(  irods::plugin_context& _ctx ) {

        return ERROR( SYS_NOT_SUPPORTED, "azureFileMkdirPlugin" );

    } // azureFileMkdirPlugin

    // =-=-=-=-=-=-=-
    // interface for POSIX rmdir
    irods::error azureFileRmdirPlugin(  irods::plugin_context& _ctx ) {

        return ERROR( SYS_NOT_SUPPORTED, "azureFileRmdirPlugin" );
    } // azureFileRmdirPlugin

    // =-=-=-=-=-=-=-
    // interface for POSIX opendir
    irods::error azureFileOpendirPlugin( irods::plugin_context& _ctx ) {

        return ERROR( SYS_NOT_SUPPORTED, "azureFileOpendirPlugin" );
    } // azureFileOpendirPlugin

    // =-=-=-=-=-=-=-
    // interface for POSIX closedir
    irods::error azureFileClosedirPlugin( irods::plugin_context& _ctx) {

        return ERROR( SYS_NOT_SUPPORTED, "azureFileClosedirPlugin" );
    } // azureFileClosedirPlugin

    // =-=-=-=-=-=-=-
    // interface for POSIX readdir
    irods::error azureFileReaddirPlugin( irods::plugin_context& _ctx,
                                        struct rodsDirent**     _dirent_ptr ) {

        return ERROR( SYS_NOT_SUPPORTED, "azureFileReaddirPlugin" );
    } // azureFileReaddirPlugin

    // =-=-=-=-=-=-=-
    // interface for POSIX readdir
    irods::error azureFileRenamePlugin( irods::plugin_context& _ctx,
                                       const char*         _new_file_name ) {

        return ERROR( SYS_NOT_SUPPORTED, "azureFileRenamePlugin" );
    } // azureFileRenamePlugin

    
    // interface to determine free space on a device given a path
    irods::error azureFileGetFsFreeSpacePlugin(
        irods::plugin_context& _ctx ){
        return ERROR( SYS_NOT_SUPPORTED, "azureFileGetFsFreeSpacePlugin" );
    } // azureFileGetFsFreeSpacePlugin


    // =-=-=-=-=-=-=-
    // azureStageToCache - This routine is for testing the TEST_STAGE_FILE_TYPE.
    // Just copy the file from filename to cacheFilename. optionalInfo info
    // is not used.
    irods::error azureStageToCachePlugin(
        irods::plugin_context& _ctx,
        const char*            _cache_file_name ) {

        irods::error result = SUCCESS();

        // check incoming parameters
        try {
           const utility::string_t connection = createConnectionString(_ctx);

           irods::file_object_ptr file_obj = boost::dynamic_pointer_cast< irods::file_object >( _ctx.fco() );
           std::string container = getHashedContainerName(file_obj->physical_path());
           std::string file_name = getFileName(file_obj->physical_path());

           // The old code allows user to set a mode.  We should now be doing this.
           bool status = getTheFile(container, 
                                   file_name,
                                   connection, 
                                   _cache_file_name, 
                                   file_obj->mode()); 
           if(status) {
               result =  ERROR( status, "azureStageToCachePlugin - error in getTheFile");
           }
        } catch ( const irods::exception& e) {
           irods::log( irods::error(e) );
           result = ERROR( SYS_INVALID_INPUT_PARAM, e.what());
        }

        return result;
    } // azureStageToCachePlugin

    irods::error unlink_for_overwrite(
        std::string            container,
        std::string            file_name,
        utility::string_t      connection,
        irods::plugin_context& _ctx) {

        irods::file_object_ptr fobj = boost::dynamic_pointer_cast<
            irods::file_object>(
                    _ctx.fco());
        std::string resc_name;
        irods::error ret = _ctx.prop_map().get<std::string>(
                irods::RESOURCE_NAME,
                resc_name );
        if( !ret.ok() ) {
            return PASS( ret );
        }

        irods::hierarchy_parser hp;
        hp.set_string(fobj->resc_hier());
        if(hp.resc_in_hier(resc_name)) {

            std::string vault_path;
            _ctx.prop_map().get<std::string>(irods::RESOURCE_PATH, vault_path);

            // only delete the file if we dont' have a vault_path
            if (fobj->physical_path().find(vault_path) == std::string::npos) {
                rodsLog(LOG_DEBUG6, 
                   "Calling deleteTheFile in unlink_for_overwrite: container %s file_name %s" , 
                   container.c_str(), file_name.c_str());
                deleteTheFile(
                    container,
                    file_name,
                    connection);
            } 

        }

        return SUCCESS();

    } // unlink_for_overwrite

    // =-=-=-=-=-=-=-
    // azureSyncToArch - This routine is for testing the TEST_STAGE_FILE_TYPE.
    // Just copy the file from cacheFilename to filename. optionalInfo info
    // is not used.
    irods::error azureSyncToArchPlugin( 
        irods::plugin_context& _ctx,
        const char*            _cache_file_name ) {
        std::string storage_connection; 

        irods::error prop_ret;
        std::string my_account;
        std::string my_key;
        std::ostringstream out_stream;
        irods::error result = SUCCESS();

        try {
           // Get the connection
           const utility::string_t connection = createConnectionString(_ctx);
           irods::file_object_ptr file_obj = boost::dynamic_pointer_cast< irods::file_object >( _ctx.fco() );

           // the hashed container name is the hash of the physical path not including the file name.
           std::string container = getHashedContainerName(file_obj->physical_path());
           rodsLog(LOG_DEBUG6, "HashedContainer name is %s", container.c_str());
           std::string file_name = getFileName(file_obj->physical_path());
           std::string azureFilePath = container + "/" + file_name;

           irods::error err = unlink_for_overwrite(container, file_name, connection, _ctx);
           if(!err.ok()) {
              irods::log(err);
           }

    
           result = putTheFile( connection,
                                (const char *)_cache_file_name, 
                                file_name,
                                file_obj->physical_path().c_str(),
                                container,
                                _ctx);
           if (result.ok()) {
              // We want to set the new physical path
              file_obj->physical_path(std::string(container + "/" + file_name));
           } else {
              result = ERROR( UNIV_MSS_SYNCTOARCH_ERR, 
                              std::string("azureSyncToArchPlugin - error in putTheFile" ));
           }
        } catch ( const irods::exception& e) {
           irods::log( irods::error(e) );
           result = ERROR( UNIV_MSS_SYNCTOARCH_ERR, e.what());
        }

        return result;

    } // azureSyncToArchPlugin

    // =-=-=-=-=-=-=-
    // redirect_get - code to determine redirection for get operation
    irods::error azureRedirectCreate( 
                      irods::plugin_property_map& _prop_map,
                      irods::file_object_ptr        _file_obj,
                      const std::string&             _resc_name, 
                      const std::string&             _curr_host, 
                      float&                         _out_vote ) {

        irods::error result = SUCCESS();

        // =-=-=-=-=-=-=-
        // determine if the resource is down 
        int resc_status = 0;

        irods::error get_ret = _prop_map.get< int >( irods::RESOURCE_STATUS, resc_status );
        if((result = ASSERT_PASS(get_ret, "azureRedirectCreate - failed to get 'status' property")).ok()) {

           // =-=-=-=-=-=-=-
           // if the status is down, vote no.
           if( INT_RESC_STATUS_DOWN == resc_status ) {
               _out_vote = 0.0;
           } else {
   
              // =-=-=-=-=-=-=-
              // get the resource host for comparison to curr host
              std::string host_name;
              get_ret = _prop_map.get< std::string >( irods::RESOURCE_LOCATION, host_name );
              if((result = ASSERT_PASS(get_ret, "azureRedirectCreate - failed to get 'location' prop")).ok()) {
                 // vote higher if we are on the same host
                 if( _curr_host == host_name ) {
                     _out_vote = 1.0;
                 } else {
                     _out_vote = 0.5;
                 }
              }
           }
        }
        return result;

    } // azureRedirectCreate

    /// =-=-=-=-=-=-=-
    /// @brief given a property map and file object, attempt to fetch it from 
    ///        the Azure system as it may be replicated under the covers.  we then
    ///        regsiter the archive version as a proper replica
    irods::error register_archive_object(
        irods::plugin_context&  _ctx,
        irods::file_object_ptr           _file_obj ) {

        irods:: error ret;
        std::vector< irods::physical_object > objs;
        std::string obj_id;
        int status;

        try {
           // =-=-=-=-=-=-=-
           // get the name of this resource
           std::string resc_name;
           ret = _ctx.prop_map().get< std::string >( irods::RESOURCE_NAME, resc_name );
           if( !ret.ok() ) {
               return PASS( ret );
           }

           const utility::string_t connection = createConnectionString(_ctx);
           std::string container = getHashedContainerName(_file_obj->physical_path());
           std::string file_name = getFileName(_file_obj->physical_path());

           // =-=-=-=-=-=-=-
           // first scan for a repl with this resource in the
           // hierarchy, if there is one then no need to continue
           // =-=-=-=-=-=-=-
           objs = _file_obj->replicas();
           std::vector< irods::physical_object >::iterator itr = objs.begin();
           for ( ; itr != objs.end(); ++itr ) {
               if( std::string::npos != itr->resc_hier().find( resc_name ) ) {
                   return SUCCESS();
               }

           } // for itr

           // =-=-=-=-=-=-=-
           // get the repl policy to determine if we need to check for an archived
           // replica and if so register it
           std::string repl_policy;
           ret = _ctx.prop_map().get< std::string >( REPL_POLICY_KEY, repl_policy );
           if( !ret.ok() ) {
               return ERROR( INVALID_OBJECT_NAME, "object not found on the archive" );
           }

           // =-=-=-=-=-=-=-
           // check the repl policy
           // TODO

           // =-=-=-=-=-=-=-
           // search for a phypath with ONE separator thatb is not the beginning, this should be the Azure id
           std::string virt_sep = irods::get_virtual_path_separator();
           for ( itr  = objs.begin();
                 itr != objs.end(); 
                 ++itr ) {

               size_t pos = itr->path().find( virt_sep );
               if( std::string::npos != pos  && 0 != pos) {
                   obj_id = itr->path();
                   break;
               } else {
                   rodsLog(LOG_DEBUG6, "%s - [%s] is not an Azure path", __FUNCTION__, itr->path().c_str());
               }
           }

           // TODO see of obj_id is not found
           if (obj_id.empty()) {
               return ERROR(INVALID_OBJECT_NAME, "obj_id is empty");
           }

           // perform a stat on the obj id to see if it is there
           status = getTheFileStatus( container, file_name, connection );
           if ( status ) {
               return ERROR( status, "error in getTheFileStatus");

           } 
        } catch ( const irods::exception& e) {
           irods::log( irods::error(e) );
           ret = ERROR( SYS_INVALID_INPUT_PARAM, e.what());
           return ret;
        }
        // =-=-=-=-=-=-=-
        // get our parent resource
        rodsLong_t resc_id = 0;
        ret = _ctx.prop_map().get<rodsLong_t>( irods::RESOURCE_ID, resc_id );
        if( !ret.ok() ) {
            return PASS( ret );
        }

        std::string resc_hier;
        ret = resc_mgr.leaf_id_to_hier(resc_id, resc_hier);
        if( !ret.ok() ) {
            return PASS( ret );
        }

        // =-=-=-=-=-=-=-
        // get the root resc of the hier
        std::string root_resc;
        irods::hierarchy_parser parser;
        parser.set_string( resc_hier );
        parser.first_resc( root_resc );
        
        // =-=-=-=-=-=-=-
        // find the highest repl number for this data object
        int max_repl_num = 0;
        for ( auto itr  = objs.begin();
              itr != objs.end(); 
              ++itr ) {
            if( itr->repl_num() > max_repl_num ) {
                max_repl_num = itr->repl_num();
            
            }

        } // for itr

        // =-=-=-=-=-=-=-
        // grab the first physical object to reference
        // for the various properties in the obj info
        // physical object to mine for various properties
        auto itr = objs.begin();

        // =-=-=-=-=-=-=-
        // build out a dataObjInfo_t struct for use in the call
        // to rsRegDataObj
        dataObjInfo_t dst_data_obj;
        bzero( &dst_data_obj, sizeof( dst_data_obj ) );

        resc_mgr.hier_to_leaf_id(resc_hier.c_str(), dst_data_obj.rescId);
        strncpy( dst_data_obj.objPath,       itr->name().c_str(),             MAX_NAME_LEN );
        strncpy( dst_data_obj.rescName,      root_resc.c_str(),               NAME_LEN );
        strncpy( dst_data_obj.rescHier,      resc_hier.c_str(),               MAX_NAME_LEN );
        strncpy( dst_data_obj.dataType,      itr->type_name( ).c_str(),       NAME_LEN );
        dst_data_obj.dataSize = itr->size( );
        strncpy( dst_data_obj.chksum,        itr->checksum( ).c_str(),        NAME_LEN );
        strncpy( dst_data_obj.version,       itr->version( ).c_str(),         NAME_LEN );
        strncpy( dst_data_obj.filePath,      obj_id.c_str(),                  MAX_NAME_LEN );
        strncpy( dst_data_obj.dataOwnerName, itr->owner_name( ).c_str(),      NAME_LEN );
        strncpy( dst_data_obj.dataOwnerZone, itr->owner_zone( ).c_str(),      NAME_LEN );
        dst_data_obj.replNum    = max_repl_num+1;
        dst_data_obj.replStatus = itr->is_dirty( );
        strncpy( dst_data_obj.statusString,  itr->status( ).c_str(),          NAME_LEN );
        dst_data_obj.dataId = itr->id(); 
        dst_data_obj.collId = itr->coll_id(); 
        dst_data_obj.dataMapId = 0; 
        dst_data_obj.flags     = 0; 
        strncpy( dst_data_obj.dataComments,  itr->r_comment( ).c_str(),       MAX_NAME_LEN );
        strncpy( dst_data_obj.dataMode,      itr->mode( ).c_str(),            SHORT_STR_LEN );
        strncpy( dst_data_obj.dataExpiry,    itr->expiry_ts( ).c_str(),       TIME_LEN );
        strncpy( dst_data_obj.dataCreate,    itr->create_ts( ).c_str(),       TIME_LEN );
        strncpy( dst_data_obj.dataModify,    itr->modify_ts( ).c_str(),       TIME_LEN );

        // =-=-=-=-=-=-=-
        // manufacture a src data obj
        dataObjInfo_t src_data_obj;
        memcpy( &src_data_obj, &dst_data_obj, sizeof( dst_data_obj ) );
        src_data_obj.replNum = itr->repl_num();
        strncpy( src_data_obj.filePath, itr->path().c_str(),       MAX_NAME_LEN );
        strncpy( src_data_obj.rescHier, itr->resc_hier().c_str(),  MAX_NAME_LEN );

        // =-=-=-=-=-=-=-
        // repl to an existing copy
        regReplica_t reg_inp;
        bzero( &reg_inp, sizeof( reg_inp ) );
        reg_inp.srcDataObjInfo  = &src_data_obj;
        reg_inp.destDataObjInfo = &dst_data_obj;
        status = rsRegReplica( _ctx.comm(), &reg_inp );
        if( status < 0 ) {
            return ERROR( status, "failed to register data object" );
        }
        
        // =-=-=-=-=-=-=-
        // we need to make a physical object and add it to the file_object
        // so it can get picked up for the repl operation
        irods::physical_object phy_obj = (*itr);
        phy_obj.resc_hier( dst_data_obj.rescHier );
        phy_obj.repl_num( dst_data_obj.replNum );
        objs.push_back( phy_obj );
        _file_obj->replicas( objs );

        // =-=-=-=-=-=-=-
        // repave resc hier in file object as it is
        // what is used to determine hierarchy in
        // the compound resource
        _file_obj->resc_hier( dst_data_obj.rescHier );
        _file_obj->physical_path( dst_data_obj.filePath );

        return SUCCESS();

    } // register_archive_object
    
    // =-=-=-=-=-=-=-
    // redirect_get - code to determine redirection for get operation
    irods::error azureRedirectOpen( 
        irods::plugin_context&  _ctx,
        irods::plugin_property_map& _prop_map,
        irods::file_object_ptr      _file_obj,
        const std::string&          _resc_name, 
        const std::string&          _curr_host, 
        float&                      _out_vote ) {
        irods::error result = SUCCESS();

        // =-=-=-=-=-=-=-
        // determine if the resource is down 
        int resc_status = 0;
        irods::error get_ret = _prop_map.get< int >( irods::RESOURCE_STATUS, resc_status );
        if((result = ASSERT_PASS(get_ret, "azureRedirectOpen - failed to get 'status' property")).ok()) {

           // =-=-=-=-=-=-=-
           // if the status is down, vote no.
           if( INT_RESC_STATUS_DOWN == resc_status ) {
               _out_vote = 0.0;
           } else {
   
              // =-=-=-=-=-=-=-
              // get the resource host for comparison to curr host
              std::string host_name;
              get_ret = _prop_map.get< std::string >( irods::RESOURCE_LOCATION, host_name );
              if((result = ASSERT_PASS(get_ret, "azureRedirectOpen - failed to get 'location' prop")).ok()) {
                 // =-=-=-=-=-=-=-
                 
                 // consider registration of object on Azure if it is not already but only if we are on the current host
                 if ( _curr_host == host_name ) {
	             get_ret = register_archive_object( 
				       _ctx,
				       _file_obj );

	             if((result = ASSERT_PASS(get_ret, "azureRedirectOpen - register_archive_object failed")).ok()) {
	                 // =-=-=-=-=-=-=-
	                 // vote higher if we are on the same host
		         if( _curr_host == host_name ) {
		             _out_vote = 1.0;
		         } //else {
		         //    _out_vote = 0.5;
	                 //}
                     }
                 
                 }
              }
           }
        } 
        return result;

    } // azureRedirectOpen

    // =-=-=-=-=-=-=-
    // used to allow the resource to determine which host
    // should provide the requested operation
    irods::error azureRedirectPlugin( 
        irods::plugin_context&  _ctx,
        const std::string*                _opr,
        const std::string*                _curr_host,
        irods::hierarchy_parser*         _out_parser,
        float*                            _out_vote ) {

        irods::error result = SUCCESS();

        // =-=-=-=-=-=-=-
        // check the context validity
        irods::error ret = _ctx.valid< irods::file_object >(); 
        if((result = ASSERT_PASS(ret, "Invalid parameters or physical path.")).ok()) {
    
           // =-=-=-=-=-=-=-
           // check incoming parameters
           if( !_opr  ) {
               result =  ERROR( -1, "azureRedirectPlugin - null operation" );
           } else if( !_curr_host ) {
               result =  ERROR( -1, "azureRedirectPlugin - null current host" );
           } else  if( !_out_parser ) {
               result =  ERROR( -1, "azureRedirectPlugin - null outgoing hier parser" );
           } else if( !_out_vote ) {
               result =  ERROR( -1, "azureRedirectPlugin - null outgoing vote" );
           } else {
              // =-=-=-=-=-=-=-
              // cast down the chain to our understood object type
              irods::file_object_ptr file_obj = boost::dynamic_pointer_cast< irods::file_object >( _ctx.fco() );
      
              // =-=-=-=-=-=-=-
              // get the name of this resource
              std::string resc_name;
              ret = _ctx.prop_map().get< std::string >( irods::RESOURCE_NAME, resc_name );
              if((result = ASSERT_PASS(ret, "azureRedirectPlugin - failed in get property for name")).ok()) {
                 // =-=-=-=-=-=-=-
                 // add ourselves to the hierarchy parser by default
                 _out_parser->add_child( resc_name );
         
                 // =-=-=-=-=-=-=-
                 // test the operation to determine which choices to make
                 if( irods::OPEN_OPERATION == (*_opr) ) {
                     // =-=-=-=-=-=-=-
                     // call redirect determination for 'get' operation
                     result =  
                        azureRedirectOpen( _ctx, _ctx.prop_map(), file_obj, resc_name, (*_curr_host), (*_out_vote));
                 } else if( irods::CREATE_OPERATION == (*_opr) ) {
                     // =-=-=-=-=-=-=-
                     // call redirect determination for 'create' operation
                     result =  
                        azureRedirectCreate( _ctx.prop_map(), file_obj, resc_name, (*_curr_host), (*_out_vote));
                 } else {
                   result = ERROR(-1, "azureRedirectPlugin - operation not supported");
                 }
              }
           }
        } 
        return result;

    } // azureRedirectPlugin

    class azure_resource : public irods::resource {
    public:
        azure_resource( const std::string& _inst_name,
                const std::string& _context ) :
            irods::resource( _inst_name, _context ) {
            // =-=-=-=-=-=-=-
            // parse context string into property pairs assuming a ; as a separator
            std::vector< std::string > props;
            rodsLog( LOG_DEBUG, "azure context: %s", _context.c_str());
            irods::kvp_map_t kvp;
            irods::parse_kvp_string(
                _context,
                kvp );

            // =-=-=-=-=-=-=-
            // copy the properties from the context to the prop map
            irods::kvp_map_t::iterator itr = kvp.begin();
            for( ; itr != kvp.end(); ++itr ) {
                properties_.set< std::string >( 
                    itr->first,
                    itr->second );
                    
            } // for itr

            // =-=-=-=-=-=-=-
            // check for certain properties
            std::string azure_account;
            irods::error prop_ret = properties_.get< std::string >( AZURE_ACCOUNT_FILE, azure_account );
            if (!prop_ret.ok()) {
                std::stringstream msg;
                rodsLog( LOG_ERROR, "prop_map has no azure account file" );
            }

        } // ctor

        irods::error need_post_disconnect_maintenance_operation( bool& _b ) {
            _b = false;
            return SUCCESS();
        }


        // =-=-=-=-=-=-=-
        // 3b. pass along a functor for maintenance work after
        //     the client disconnects, uncomment the first two lines for effect.
        irods::error post_disconnect_maintenance_operation( irods::pdmo_type& _op  ) {
            return SUCCESS();
        }


        ~azure_resource() {
        }

    }; // class azure_resource

    // =-=-=-=-=-=-=-
    // Create the plugin factory function which will return a microservice
    // table entry containing the microservice function pointer, the number
    // of parameters that the microservice takes and the name of the micro
    // service.  this will be called by the plugin loader in the irods server
    // to create the entry to the table when the plugin is requested.
    extern "C"
    irods::resource* plugin_factory(const std::string& _inst_name, const std::string& _context) {
        azure_resource* resc = new azure_resource( _inst_name, _context );
        using namespace irods;
        using namespace std;
        resc->add_operation(
            RESOURCE_OP_CREATE,
            function<error(plugin_context&)>(
                azureFileCreatePlugin));
        resc->add_operation(
            irods::RESOURCE_OP_OPEN,
            function<error(plugin_context&)>(
                azureFileOpenPlugin));
        resc->add_operation<void*,int>(
            irods::RESOURCE_OP_READ,
            std::function<
                error(irods::plugin_context&,void*,int)>(
                    azureFileReadPlugin));
        resc->add_operation<void*,int>(
            irods::RESOURCE_OP_WRITE,
            function<error(plugin_context&,void*,int)>(
                azureFileWritePlugin));
        resc->add_operation(
            RESOURCE_OP_CLOSE,
            function<error(plugin_context&)>(
                azureFileClosePlugin));
        resc->add_operation(
            irods::RESOURCE_OP_UNLINK,
            function<error(plugin_context&)>(
                azureFileUnlinkPlugin));
        resc->add_operation<struct stat*>(
            irods::RESOURCE_OP_STAT,
            function<error(plugin_context&, struct stat*)>(
                azureFileStatPlugin));
        resc->add_operation(
            irods::RESOURCE_OP_MKDIR,
            function<error(plugin_context&)>(
                azureFileMkdirPlugin));
        resc->add_operation(
            irods::RESOURCE_OP_OPENDIR,
            function<error(plugin_context&)>(
                azureFileOpendirPlugin));
        resc->add_operation<struct rodsDirent**>(
            irods::RESOURCE_OP_READDIR,
            function<error(plugin_context&,struct rodsDirent**)>(
                azureFileReaddirPlugin));
        resc->add_operation<const char*>(
            irods::RESOURCE_OP_RENAME,
            function<error(plugin_context&, const char*)>(
                azureFileRenamePlugin));
        resc->add_operation(
            irods::RESOURCE_OP_FREESPACE,
            function<error(plugin_context&)>(
                azureFileGetFsFreeSpacePlugin));
        resc->add_operation<long long, int>(
            irods::RESOURCE_OP_LSEEK,
            function<error(plugin_context&, long long, int)>(
                azureFileLseekPlugin));
        resc->add_operation(
            irods::RESOURCE_OP_CLOSEDIR,
            function<error(plugin_context&)>(
                azureFileClosedirPlugin));
        resc->add_operation<const char*>(
            irods::RESOURCE_OP_STAGETOCACHE,
            function<error(plugin_context&, const char*)>(
                azureStageToCachePlugin));
        resc->add_operation<const char*>(
            irods::RESOURCE_OP_SYNCTOARCH,
            function<error(plugin_context&, const char*)>(
                azureSyncToArchPlugin));
        resc->add_operation(
            irods::RESOURCE_OP_REGISTERED,
            function<error(plugin_context&)>(
                azureRegisteredPlugin));
        resc->add_operation(
            irods::RESOURCE_OP_UNREGISTERED,
            function<error(plugin_context&)>(
                azureUnregisteredPlugin));
        resc->add_operation(
            irods::RESOURCE_OP_MODIFIED,
            function<error(plugin_context&)>(
                azureModifiedPlugin));
        resc->add_operation(
            irods::RESOURCE_OP_RMDIR,
            function<error(plugin_context&)>(
                azureFileRmdirPlugin));
        resc->add_operation<const std::string*, const std::string*, irods::hierarchy_parser*, float*>(
            irods::RESOURCE_OP_RESOLVE_RESC_HIER,
            function<error(plugin_context&,const std::string*, const std::string*, irods::hierarchy_parser*, float*)>(
                azureRedirectPlugin));

        // set some properties necessary for backporting to iRODS legacy code
        resc->set_property< int >( irods::RESOURCE_CHECK_PATH_PERM, DO_CHK_PATH_PERM );
        resc->set_property< int >( irods::RESOURCE_CREATE_PATH,     CREATE_PATH );
        return dynamic_cast<irods::resource *>( resc );

    } // plugin_factory



