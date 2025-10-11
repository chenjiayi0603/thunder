/*
* rsa_encrypt.h
*
*  Created on: 2019-12-26
*      Author: frank
*/
#ifndef RSA_ENCRYPT_H_
#define RSA_ENCRYPT_H_
#include <openssl/rsa.h>
#include <openssl/pem.h>
#include<string>
#include<memory.h>
#include<iostream>
#include "openssl/err.h"
using namespace std;
#ifdef __cplusplus
extern "C" {
#endif

#define PUBLICKEY "publicKey.pem"
#define PRIVATEKEY "privateKey.pem"
#define KEY_LENGTH  2048 
//int iPadding = RSA_PKCS1_PADDING;
//int i_encrypt_len = 0;
#define PUB_EXP     3 

 
//��ȡ��Կ�ļ��й�Կ�� �ڴ�rsa �ṹ��
RSA *readRsaPublicKeyFromFile(char* filePathPem);
 
RSA *readRsaPrivateKeyFromFile(char* filePathPem, char* passwd);
 

/**
* @summary ���ɹ�Կ�Ե��ڴ�
* @param data strKey��strKey[0] ��Կ�� strKey[1] ˽Կ
* @return �Ƿ���ɹ� -1 ʧ�ܣ� 0 �ɹ�
*/
int GenerateRsaKeyToMem(string strKey[]);
 

/**
* @summary ���ַ�����Կת��Ϊ rsa�ṹ��
* @param key ��Կ�ַ���
* @param flag  1 publickey ;0 privatekey
* @return NUll ʧ�ܣ����򷵻�rsa �ṹ����Կ
*/
RSA* createRsaFromKeyStr(unsigned char* key, int flag);
 



/**
* @summary ��Կ��������
* @param data ����������
* @param data_len ���������ݳ���
* @param rsa ��Կ
* @param encrypted ���ܺ�����
* @param iPadding  ���뷽ʽ
* @return �Ƿ���ɹ� -1 ʧ�ܣ� >0  ���ܺ����ݳ���
*/
int publicEncrypt(unsigned char* pData, int iDataLen, RSA *pRsa, unsigned char* pEncryptData, int iPadding);
 
/**
* @summary ˽Կ��������
* @param pEncryptData ����������
* @param iEncryptDataLen  ������������
* @param rsa ��Կ
* @param pDecryptData  ���ܺ�����
* @param iPadding  ���뷽ʽ
* @return �Ƿ���ɹ� -1 ʧ�ܣ� >0  ���ܺ����ݳ���
*/
int privateDecrypt(unsigned char* pEncryptData, int iEncryptDataLen, RSA *pRsa, unsigned char* pDecryptData, int iPadding);
 


/**
* @summary ˽Կ��������
* @param pEncryptData ����������
* @param iEncryptDataLen  ������������
* @param rsa ��Կ
* @param pDecryptData  ���ܺ�����
* @param iPadding  ���뷽ʽ
* @return �Ƿ���ɹ� -1 ʧ�ܣ� >0  ���ܺ����ݳ���
*/
int privateEncrypt(unsigned char* pData, int iDataLen, RSA *pRsa, unsigned char* pEncryptData, int iPadding);
 
/**
* @summary ��Կ��������
* @param pEncryptData ����������
* @param iEncryptDataLen  ������������
* @param rsa ��Կ
* @param pDecryptData  ���ܺ�����
* @param iPadding  ���뷽ʽ
* @return �Ƿ���ɹ� -1 ʧ�ܣ� >0  ���ܺ����ݳ���
*/
int publicDecrypt(unsigned char* pEncryptData, int iEncryptDataLen, RSA *pRsa, unsigned char* pDecryptData, int iPadding);
 

void printLastError(char *msg);
 
void demoTest();

#ifdef __cplusplus
}
#endif

#endif /* RSA_ENCRYPT_H_ */
