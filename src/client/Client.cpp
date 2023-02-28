#include <iostream>
#include <cstring>
#include <openssl/pem.h>

#include "Client.h"
#include "../packet/Generic.h"
#include "../packet/Login.h"
#include "../packet/Logout.h"
#include "../packet/Result.h"
#include "../packet/Upload.h"
#include "../security/DiffieHellman.h"
#include "../security/Sha512.h"
#include "../security/DigitalSignature.h"
#include "../security/AesCbc.h"

using namespace std;

Client::Client(bool verbose) { 
    
    m_verbose = verbose; 
    m_socket = nullptr;
    m_long_term_key = nullptr;
}

Client::~Client() {

    delete m_socket;
    EVP_PKEY_free(m_long_term_key);
    CertificateStore::deleteStore();

    // overwrite session key and hmac key
    #pragma optimize("", off)
    memset(m_session_key, 0, sizeof(m_session_key));
    memset(m_hmac_key, 0, sizeof(m_hmac_key));
    #pragma optimize("", on)
}

int Client::login() {

    // generate the ephemeral key (that contains private and public keys)
    DiffieHellman dh;
    EVP_PKEY* ephemeral_key = dh.generateEphemeralKey();

    // serialize ephemeral key
    uint8_t* serialized_ephemeral_key = nullptr;
    int serialized_ephemeral_key_size;
    int res = DiffieHellman::serializeKey(ephemeral_key, serialized_ephemeral_key, serialized_ephemeral_key_size);
    if (res < 0) {
        EVP_PKEY_free(ephemeral_key);
        delete[] serialized_ephemeral_key;
        return -1;
    }

    // 1.) send ephemeral key and username
    LoginM1 m1(serialized_ephemeral_key, serialized_ephemeral_key_size, m_username);
    uint8_t* serialized_packet = m1.serialize();
    res = m_socket->send(serialized_packet, LoginM1::getSize());
    delete[] serialized_packet;
    if (res < 0) {
        EVP_PKEY_free(ephemeral_key);
        delete[] serialized_ephemeral_key;
        return -2;
    }

    LOG("(Login) Ephemeral key and username sent to the server");

    // 2.) receive the result of existence of the user
    serialized_packet = new uint8_t[LoginM2::getSize()];
    res = m_socket->receive(serialized_packet, LoginM2::getSize());
    if (res < 0) {
        delete[] serialized_packet;
        EVP_PKEY_free(ephemeral_key);
        delete[] serialized_ephemeral_key;
        return -3;
    }

    LOG("(Login) Received username check result from the server");

    // check if the server found the username
    uint8_t result_check = LoginM2::deserialize(serialized_packet).result;
    delete[] serialized_packet;
    if (!result_check) {
        cerr << "[-] (Login) User not exists" << endl;
        EVP_PKEY_free(ephemeral_key);
        delete[] serialized_ephemeral_key;
        return -4;
    }

    // 3.) receive the M3 packet
    serialized_packet = new uint8_t[LoginM3::getSize()];
    res = m_socket->receive(serialized_packet, LoginM3::getSize());
    if (res < 0) {
        delete[] serialized_packet;
        EVP_PKEY_free(ephemeral_key);
        delete[] serialized_ephemeral_key;
        return -5;
    }

    LOG("(Login) Received ephemeral key, signature and certificate from the server");

    // deserialize the M3 packet
    LoginM3 m3 = LoginM3::deserialize(serialized_packet);
    delete[] serialized_packet;

    // retrieve the peer ephemeral key from the M3 packet
    EVP_PKEY* peer_ephemeral_key = DiffieHellman::deserializeKey(m3.ephemeral_key, m3.ephemeral_key_size);

    // generate the shared secret
    uint8_t* shared_secret = nullptr;
    size_t shared_secret_size;    
    res = dh.generateSharedSecret(ephemeral_key, peer_ephemeral_key, shared_secret, shared_secret_size);
    EVP_PKEY_free(ephemeral_key);
    EVP_PKEY_free(peer_ephemeral_key);
    if (res < 0) {
        #pragma optimize("", off)
        memset(shared_secret, 0, shared_secret_size);
        #pragma optimize("", on)
        delete[] shared_secret;
        delete[] serialized_ephemeral_key;
        return -6;
    }
    
    // generate the session key and hmac key
    unsigned char* keys = nullptr;
    unsigned int keys_size;
    Sha512::generate(shared_secret, shared_secret_size, keys, keys_size);
    memcpy(m_session_key, keys, 32 * sizeof(unsigned char));
    memcpy(m_hmac_key, keys + (32 * sizeof(unsigned char)), 32 * sizeof(unsigned char));
    #pragma optimize("", off)
    memset(shared_secret, 0, shared_secret_size);
    #pragma optimize("", on)
    delete[] shared_secret;
    delete[] keys;

    LOG("(Login) Generated session key and HMAC key");
    
    // prepare <g^a,g^b>
    int ephemeral_keys_buffer_size = m3.ephemeral_key_size + serialized_ephemeral_key_size;
    uint8_t* ephemeral_keys_buffer = new uint8_t[ephemeral_keys_buffer_size];
    memcpy(ephemeral_keys_buffer, serialized_ephemeral_key, serialized_ephemeral_key_size);
    memcpy(ephemeral_keys_buffer + serialized_ephemeral_key_size, m3.ephemeral_key, m3.ephemeral_key_size);
    delete[] serialized_ephemeral_key;
    
    // calculate <g^a,g^b>_s
    unsigned char* signature = nullptr;
    unsigned int signature_size;
    DigitalSignature::generate(ephemeral_keys_buffer, ephemeral_keys_buffer_size, signature, signature_size, m_long_term_key);

    // calculate {<g^a,g^b>_s}_Ksess
    unsigned char* ciphertext = nullptr;
    unsigned char* iv = nullptr;
    int ciphertext_size = 0;
    AesCbc* encryptor = new AesCbc(ENCRYPT, m_session_key);
    encryptor->run(signature, signature_size, ciphertext, ciphertext_size, iv);
    delete[] signature;
    delete encryptor;

    // retrieve and verify the certificate
    X509* server_certificate = CertificateStore::deserializeCertificate(m3.serialized_certificate, m3.serialized_certificate_size);
    CertificateStore* certificate_store = CertificateStore::getStore();
    if (!certificate_store->verify(server_certificate)) {
        X509_free(server_certificate);
        delete[] ephemeral_keys_buffer;
        delete[] ciphertext;
        delete[] iv;
        return -7;
    }

    LOG("(Login) Verified server certificate");

    // retrieve the server public key 
    EVP_PKEY* server_public_key = certificate_store->getPublicKey(server_certificate);
    X509_free(server_certificate);

    // decrypt the encrypted digital signature
    unsigned char* decrypted_signature = nullptr;
    int decrypted_signature_size = 0;
    AesCbc* decryptor = new AesCbc(DECRYPT, m_session_key);
    unsigned char* signature_iv = m3.iv; 
    decryptor->run(m3.encrypted_signature, 144 * sizeof(uint8_t), decrypted_signature, decrypted_signature_size, signature_iv);
    delete decryptor;

    // verify the signature
    bool signature_verification = DigitalSignature::verify(ephemeral_keys_buffer, ephemeral_keys_buffer_size, decrypted_signature, decrypted_signature_size, server_public_key);
    delete[] ephemeral_keys_buffer;
    delete[] decrypted_signature;
    EVP_PKEY_free(server_public_key);
    if (!signature_verification) {
        delete[] ciphertext;
        delete[] iv;
        return -8;
    }

    LOG("(Login) Verified server signature");

    // 4.) prepare and send the M4 packet
    LoginM4 m4(iv, ciphertext);
    serialized_packet = m4.serialize();
    res = m_socket->send(serialized_packet, LoginM4::getSize());
    delete[] serialized_packet;
    delete[] ciphertext;
    delete[] iv;
    if (res < 0) {
        return -9;
    }

    LOG("(Login) Sent signature to the server");

    // reset the counter
    m_counter = 0;
    return 0;
}

int Client::logout() {

    // create the M1 packet
    LogoutM1 m1(m_counter);
    // m1.print();
    uint8_t* serialized_packet = m1.serialize();

    // create generic packet
    Generic generic_m1(m_session_key, m_hmac_key, serialized_packet, COMMAND_FIELD_PACKET_SIZE);
    #pragma optimize("", off)
    memset(serialized_packet, 0, COMMAND_FIELD_PACKET_SIZE);
    #pragma optimize("", on)
    delete[] serialized_packet;
    // generic_m1.print();

    // 1.) send generic packet
    serialized_packet = generic_m1.serialize();
    int res = m_socket->send(serialized_packet, Generic::getSize(COMMAND_FIELD_PACKET_SIZE));
    delete[] serialized_packet;
    if (res < 0) {
        return -1;
    }

    incrementCounter();

    // 2.) receive the generic packet
    serialized_packet = new uint8_t[Generic::getSize(Result::getSize())];
    res = m_socket->receive(serialized_packet, Generic::getSize(Result::getSize()));
    if (res < 0) {
        // TODO: errore + delete
        delete[] serialized_packet;
        return -1;
    }

    // deserialize the generic packet and verify the fingerprint
    Generic generic_m2 = Generic::deserialize(serialized_packet, Generic::getSize(Result::getSize()));
    delete[] serialized_packet;
    // generic_m2.print();
    bool verification_res = generic_m2.verifyHMAC(m_hmac_key);
    if (!verification_res) {
        cerr << "[-] (Logout) HMAC verification failed" << endl;
        return -2;
    }

    LOG("(Logout) Received valid packet");

    // get the m2 packet
    uint8_t* plaintext = nullptr;
    int plaintext_size = 0;
    generic_m2.decryptCiphertext(m_session_key, plaintext, plaintext_size);
    Result m2 = Result::deserialize(plaintext);
    // m2.print();
    #pragma optimize("", off)
    memset(plaintext, 0, Result::getSize());
    #pragma optimize("", on)
    delete[] plaintext;

    // check if the counter is correct
    if (m2.counter != m_counter) {
        // TODO: use the goto?
        cerr << "[-] (Logout) Invalid counter" << endl;
    }

    // check if operation failed
    if (m2.command_code == REQ_SUCCESS)
        return 0;
    else if (m2.command_code == REQ_FAILED)
        return -1;

    return -2;
}

// ----------- BIAGIO -------------
// --------------------------------

// ----------- MATTEO -------------

int Client::upload(string file_name) {

    // open the file requested file
    FileManager file(file_name, READ);

    // TODO: check if the file exists with exception
    // cerr << "[-] (Upload) The requested file not exists" << endl;
    // return -1; 

    // check if the file size is zero
    if (file.getFileSize() == 0) {
        cerr << "[-] (Upload) An empty file can't be uploaded" << endl;
        return -2;
    }

    // check if the file size is over 4G
    size_t max_size = 4UL * 1024 * 1024 * 1024;
    if (file.getFileSize() > max_size) {
        cerr << "[-] (Upload) Is not possible to upload file larger than 4GB" << endl;
        return -3;
    }

    // create the M1 packet
    UploadM1 m1(m_counter, file_name, file.getFileSize());
    // m1.print();
    uint8_t* serialized_packet = m1.serialize();

    // create generic packet
    Generic generic_m1(m_session_key, m_hmac_key, serialized_packet, COMMAND_FIELD_PACKET_SIZE);
    #pragma optimize("", off)
    memset(serialized_packet, 0, COMMAND_FIELD_PACKET_SIZE);
    #pragma optimize("", on)
    delete[] serialized_packet;
    // generic_m1.print();

    // 1.) send generic packet      // TODO: fix send valgrind error
    serialized_packet = generic_m1.serialize();
    int res = m_socket->send(serialized_packet, Generic::getSize(COMMAND_FIELD_PACKET_SIZE));
    delete[] serialized_packet;
    if (res < 0) {
        return -3;
    }

    incrementCounter();

    // 2.) receive the result packet
    serialized_packet = new uint8_t[Generic::getSize(Result::getSize())];
    res = m_socket->receive(serialized_packet, Generic::getSize(Result::getSize()));
    if (res < 0) {
        delete[] serialized_packet;
        return -4;
    }

    // deserialize the generic packet and verify the fingerprint
    Generic generic_m2 = Generic::deserialize(serialized_packet, Generic::getSize(Result::getSize()));
    delete[] serialized_packet;
    // generic_m2.print();
    bool verification_res = generic_m2.verifyHMAC(m_hmac_key);
    if (!verification_res) {
        cerr << "[-] (Upload) HMAC verification failed" << endl;
        return -5;
    }

    // get the M2 packet
    uint8_t* plaintext = nullptr;
    int plaintext_size = 0;
    generic_m2.decryptCiphertext(m_session_key, plaintext, plaintext_size);
    Result m2 = Result::deserialize(plaintext);
    // m2.print();
    #pragma optimize("", off)
    memset(plaintext, 0, Result::getSize());
    #pragma optimize("", on)
    delete[] plaintext;

    incrementCounter();

    // if the request failed stop
    if (m2.command_code == REQ_FAILED) {
        cerr << "[-] (Upload) The file already exists in the cloud" << endl;
        return -6;
    }

    size_t chunk_size = file.getChunkSize();

    size_t sent_size = 0;
    uint8_t* chunk_buffer = new uint8_t[chunk_size];

    // sent all file chunks
    for (size_t i = 0; i < file.getNumOfChunks(); ++i) {
        // get the chunk size
        if (i == file.getNumOfChunks() - 1)
            chunk_size = file.getLastChunkSize();

        // read the next chunk
        file.readChunk(chunk_buffer, chunk_size);

        // create the M3+i packet
        UploadMi mi(m_counter, chunk_buffer, chunk_size);
        // mi.print();
        serialized_packet = mi.serialize();

        // create generic packet
        Generic generic_mi(m_session_key, m_hmac_key, serialized_packet, UploadMi::getSize(chunk_size));
        #pragma optimize("", off)
        memset(serialized_packet, 0, UploadMi::getSize(chunk_size));
        #pragma optimize("", on)
        delete[] serialized_packet;
        // generic_mi.print();

        // 3.) send generic packet
        serialized_packet = generic_mi.serialize();
        int res = m_socket->send(serialized_packet, Generic::getSize(UploadMi::getSize(chunk_size)));
        delete[] serialized_packet;
        if (res < 0) {
            return -1;
        }

        incrementCounter();

        // print upload status
        sent_size += chunk_size;
        cout << "[+] (Upload) Uploaded " << sent_size << "byte/" << file.getFileSize() << "byte" << endl;
    }

    // clean the buffer
    #pragma optimize("", off)
    memset(chunk_buffer, 0, chunk_size);
    #pragma optimize("", on)
    delete[] chunk_buffer;

    return 0;
}

// --------------------------------

// ---------- GIANLUCA ------------
// --------------------------------

bool Client::incrementCounter() {

    // check if renegotiation is needed
    if (m_counter == MAX_COUNTER_VALUE) {
        int res = login();
        if (res != 0)
            return false;
    } else {
        m_counter++;
    }

    return true;
}

int Client::run() {

    // --------------- INITIALIZATION ---------------

    string username, password;
    cout << "Insert username: ";
    cin >> m_username;
    cout << "Insert password: ";
    cin >> password;
    
    // sanitize username and password
    static char ok_chars[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ1234567890_-.@?!#*";
    if (strspn(m_username.c_str(), ok_chars) < strlen(m_username.c_str())) { 
        cerr << "[-] (Run) Not valid username" << endl;
        return -1;
    }
    if (strspn(password.c_str(), ok_chars) < strlen(password.c_str())) { 
        cerr << "[-] (Run) Not valid username" << endl;
        return -1;
    }
    if (m_username.length() >= 30) {
        cerr << "[-] (Run) Username too long" << endl;
        return -1;
    } 

    // open the private key PEM file
    string private_key_file = "resources/encrypted_keys/" + m_username + "_key.pem";
    BIO *bio = BIO_new_file(private_key_file.c_str(), "r");
    if (!bio) {
        cerr << "[-] (Run) Failed to open encrypted key PEM file" << endl;
        return -2;
    }
    
    // encrypt and save the long term private key
    m_long_term_key = PEM_read_bio_PrivateKey(bio, 0, 0, (void *)password.c_str());
    BIO_free(bio);

    // connect to the server
    try {
        m_socket = new CommunicationSocket(SERVER_IP, SERVER_PORT);
    } catch (const std::exception& e) {
        std::cerr << "[-] (Run) Exception: " << e.what() << std::endl;
        return -3;
    }

    // ----------------------------------------------

    int res = login();
    if (res != 0) {
        cerr << "[-] (Run) Login failed with error code " << res << endl;
        return -1;
    }
    cout << "[+] (Run) Login completed" << endl;

    while (1) {
        
        string command;
        cout << "Insert next command: ";
        cin >> command;

        if (command == "list") {

        }

        else if (command == "download") {

        }

        else if (command == "upload") {
            // get the file name
            string file_name;
            cout << "Insert the name of the file: ";
            cin >> file_name;

            // sanitize filename
            if (strspn(file_name.c_str(), ok_chars) < strlen(file_name.c_str())) { 
                cerr << "[-] (Run) Not valid file name" << endl;
                continue;
            }

            res = upload(file_name);
        }

        else if (command == "rename") {

        }

        else if (command == "delete") {

        }

        else if (command == "logout" || command == "exit") {
            res = logout();
            if (res < 0) {
                cerr << "[-] (Run) Logout failed with error code " << res << endl;
                return -1;
            } 

            cout << "[+] (Run) Logout completed" << endl;

            if (command == "exit") 
                return 1;
            return 0;
        }
        
        else if (command == "help") {
            cout << "----------- COMMANDS -----------" << endl;
            cout << "list: " << endl;
            cout << "download:" << endl;
            cout << "upload:" << endl;
            cout << "rename:" << endl;
            cout << "delete:" << endl;
            cout << "logout:" << endl;
            cout << "exit:" << endl;
            cout << "--------------------------------" << endl;
        }

        else {
            cerr << "[-] (Run) Not valid command" << endl;
        }
        
    }

    return 0;
}