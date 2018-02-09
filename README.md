# irods_resource_plugin_azure
To build the Azure Resource Plugin, you will need to have:

 - the iRODS Development Tools (irods-dev[el] and irods-runtime) installed for your platform
     http://irods.org/download

And two Microsoft specific libraries. We have forks of these libraries on the helium data commons website

 - cpprest: You can get the library code with the command 

	- git clone https://github.com/heliumdatacommons/cpprestsdk.git

The azure storage lib requires version 2.9.1 of cpprest.  In order to get the required version, including the changes needed to build a version of the library compatible with the azure resource plugin, you need to switch to the 2.9.1-helium branch of these library.  To do this the commands are:

	- git checkout 2.9.1-helium

 - azurestorage: you can get the code for this library with the command
	
	-  git clone https://github.com/heliumdatacommons/azure-storage-cpp.git

Plus a set of more or less standard linux libraries that seem to be required by cpprest and azurestorarage

 - libssl-dev[el]/libssl-devel

 - openssl-dev[el]/openssl

 - glib2-dev[el]

 - sigc++

 - libsigc++20-devel

 - glibmm24-dev[el]

 - libxml++-dev[el]

 - libxml++-dev[el]

Once you have the prereqs installed you can make the azure resource plugin as follows:

# Build lib cpprest
# Get the code:
git clone https://github.com/heliumdatacommons/cpprestsdk.git

# Switch to the required branch
git checkout 2.9.1-helium

cd /yourrootdir/ccprestsdk/Release

mkdir build.release

cd build.release

# Run cmake
/opt/irods-externals/cmake3.5.2-0/bin/cmake .. -DCMAKE_COMPILE_FOR_IRODS:BOOL=on -DBOOST_ROOT:STRING=/opt/irods-externals/boost1.60.0-0/

# Build libccprest
make cpprest

# Make sure we are linked with the correct boost:
ldd Binaries/libcpprest.so

Important note: The "make install" target for this code strips the library. You may want to sudo cp the library to /usr/local/lib by hand, but you should run make install first to be the include files get put in the right place. /usr/local/lib should look like this:

ls -l /usr/local/lib/*cpp*

lrwxrwxrwx 1 root root  17 Jan 31 14:37 /usr/local/lib/libcpprest.so -> libcpprest.so.2.9*

-rwxr-xr-x 1 root root 13M Jan 31 15:56 /usr/local/lib/libcpprest.so.2.9*

# Build the azure storage library
git clone https://github.com/heliumdatacommons/azure-storage-cpp.git

cd /yourrootdir/azure-strorage-cpp/Microsoft.WindowsAzure.Storage/

mkdir build.release

cd build.release

# Run cmake:  Set CASABLANCA_DIR to yourrootdir
CASABLANCA_DIR=/yourrootdir /opt/irods-externals/cmake3.5.2-0/bin/cmake .. -DCMAKE_COMPILE_FOR_IRODS:BOOL=on

# Make the library: ignore the compile warnings
make azurestorage

# Make sure we are linked with the correct boost:
ldd Binaries/libcpprest.so

Important note: The "make install" target for this code strips the library. You may want to sudo cp the library to /usr/local/lib by hand, but you should run make install first to be the include files get put in the right place. /usr/local/lib should look like this:

ls -l /usr/local/lib/*azure*

lrwxrwxrwx 1 root root  22 Jan 31 15:33 /usr/local/lib/libazurestorage.so.3 -> libazurestorage.so.3.1*

-rwxr-xr-x 1 root root 21M Jan 31 15:54 /usr/local/lib/libazurestorage.so.3.1*

# Build the azure storage plugin
git clone https://github.com/heliumdatacommons/irods_resource_plugin_azure.git

cd irods_resource_plugin_azure

mkdir build.release

cd build.release

/opt/irods-externals/cmake3.5.2-0/bin/cmake .. -DCRYPTO_LIBDIR:STRING=/usr/lib64

make package

# Install the package depending on your package manager
sudo dpkg -i irods-resource-plugin-azure_2.2.0~xenial_amd64.deb

sudo rpm -i --force irods-resource-plugin-azure-2.2.0-1.x86_64.rpm

# Configuring the resource
The azure driver is configured as a normal irods compound resource. The context string must have a variable called AZURE_ACCOUNT_FILE.  This variable specifies the location (readable by the irods server) of the azure account file. This file must have exactly 2 lines: the first line is a valid azure account and the second line is the valid key for the account.
