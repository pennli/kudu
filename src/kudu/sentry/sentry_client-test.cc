// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "kudu/sentry/sentry_client.h"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kudu/rpc/sasl_common.h"
#include "kudu/security/test/mini_kdc.h"
#include "kudu/sentry/mini_sentry.h"
#include "kudu/sentry/sentry_policy_service_types.h"
#include "kudu/thrift/client.h"
#include "kudu/util/net/net_util.h"
#include "kudu/util/status.h"
#include "kudu/util/test_macros.h"
#include "kudu/util/test_util.h"

using std::set;
using std::string;
using std::unique_ptr;
using std::vector;

namespace kudu {
namespace sentry {

class SentryClientTest : public KuduTest,
                         public ::testing::WithParamInterface<bool> {
 public:
  bool KerberosEnabled() const {
    return GetParam();
  }

  void SetUp() override {
    bool enable_kerberos = KerberosEnabled();

    thrift::ClientOptions sentry_client_opts;

    sentry_.reset(new MiniSentry());
    if (enable_kerberos) {
      kdc_.reset(new MiniKdc());
      ASSERT_OK(kdc_->Start());

      // Create a service principal for Sentry, and configure it to use it.
      string spn = "sentry/127.0.0.1@KRBTEST.COM";
      string ktpath;
      ASSERT_OK(kdc_->CreateServiceKeytab("sentry/127.0.0.1", &ktpath));

      ASSERT_OK(rpc::SaslInit());
      sentry_->EnableKerberos(kdc_->GetEnvVars()["KRB5_CONFIG"], spn, ktpath);

      ASSERT_OK(kdc_->CreateUserPrincipal("kudu"));
      ASSERT_OK(kdc_->Kinit("kudu"));
      ASSERT_OK(kdc_->SetKrb5Environment());
      sentry_client_opts.enable_kerberos = true;
      sentry_client_opts.service_principal = "sentry";
    }
    ASSERT_OK(sentry_->Start());

    sentry_client_.reset(new SentryClient(sentry_->address(),
                                          sentry_client_opts));
    ASSERT_OK(sentry_client_->Start());
  }

  void TearDown() override {
    ASSERT_OK(sentry_client_->Stop());
    ASSERT_OK(sentry_->Stop());
  }

 protected:
  unique_ptr<SentryClient> sentry_client_;
  unique_ptr<MiniSentry> sentry_;
  unique_ptr<MiniKdc> kdc_;
};
INSTANTIATE_TEST_CASE_P(KerberosEnabled, SentryClientTest, ::testing::Bool());

TEST_P(SentryClientTest, TestMiniSentryLifecycle) {
  // Create an HA Sentry client and ensure it automatically reconnects after service interruption.
  thrift::HaClient<SentryClient> client;
  thrift::ClientOptions sentry_client_opts;
  if (KerberosEnabled()) {
    sentry_client_opts.enable_kerberos = true;
    sentry_client_opts.service_principal = "sentry";
  }

  ASSERT_OK(client.Start(vector<HostPort>({sentry_->address()}),
                         sentry_client_opts));
  auto smoketest = [&]() -> Status {
    return client.Execute([](SentryClient* client) -> Status {
        ::sentry::TCreateSentryRoleRequest create_req;
        create_req.requestorUserName = "test-admin";
        create_req.roleName = "test-role";
        RETURN_NOT_OK(client->CreateRole(create_req));

        ::sentry::TDropSentryRoleRequest drop_req;
        drop_req.requestorUserName = "test-admin";
        drop_req.roleName = "test-role";
        RETURN_NOT_OK(client->DropRole(drop_req));
        return Status::OK();
    });
  };

  ASSERT_OK(smoketest());

  ASSERT_OK(sentry_->Stop());
  ASSERT_OK(sentry_->Start());
  ASSERT_OK(smoketest());

  ASSERT_OK(sentry_->Pause());
  ASSERT_OK(sentry_->Resume());
  ASSERT_OK(smoketest());
}

// Basic functionality test of the Sentry client. The goal is not an exhaustive
// test of Sentry's role handling, but instead verification that the client can
// communicate with the Sentry service, and errors are converted to Status
// instances.
TEST_P(SentryClientTest, TestCreateDropRole) {

  { // Create a role
    ::sentry::TCreateSentryRoleRequest req;
    req.requestorUserName = "test-admin";
    req.roleName = "viewer";
    ASSERT_OK(sentry_client_->CreateRole(req));

    // Attempt to create the role again.
    Status s = sentry_client_->CreateRole(req);
    ASSERT_TRUE(s.IsAlreadyPresent()) << s.ToString();
  }

  { // Attempt to create a role as a non-admin user.
    ::sentry::TCreateSentryRoleRequest req;
    req.requestorUserName = "joe-interloper";
    req.roleName = "fuzz";
    Status s = sentry_client_->CreateRole(req);
    ASSERT_TRUE(s.IsNotAuthorized()) << s.ToString();
  }

  { // Attempt to drop the role as a non-admin user.
    ::sentry::TDropSentryRoleRequest req;
    req.requestorUserName = "joe-interloper";
    req.roleName = "viewer";
    Status s = sentry_client_->DropRole(req);
    ASSERT_TRUE(s.IsNotAuthorized()) << s.ToString();
  }

  { // Drop the role
    ::sentry::TDropSentryRoleRequest req;
    req.requestorUserName = "test-admin";
    req.roleName = "viewer";
    ASSERT_OK(sentry_client_->DropRole(req));

    // Attempt to drop the role again.
    Status s = sentry_client_->DropRole(req);
    ASSERT_TRUE(s.IsNotFound()) << s.ToString();
  }
}

// Similar to above test to verify that the client can communicate with the
// Sentry service to list privileges, and errors are converted to Status
// instances.
TEST_P(SentryClientTest, TestListPrivilege) {
  // Attempt to access Sentry privileges by a non admin user.
  ::sentry::TSentryAuthorizable authorizable;
  authorizable.server = "server";
  authorizable.db = "db";
  authorizable.table = "table";
  ::sentry::TListSentryPrivilegesRequest request;
  request.requestorUserName = "joe-interloper";
  request.authorizableHierarchy = authorizable;
  request.__set_principalName("viewer");
  ::sentry::TListSentryPrivilegesResponse response;
  Status s = sentry_client_->ListPrivilegesByUser(request, &response);
  ASSERT_TRUE(s.IsNotAuthorized()) << s.ToString();

  // Attempt to access Sentry privileges by a user without
  // group mapping.
  request.requestorUserName = "user-without-mapping";
  s = sentry_client_->ListPrivilegesByUser(request, &response);
  ASSERT_TRUE(s.IsNotAuthorized()) << s.ToString();

  // Attempt to access Sentry privileges of a non-exist user.
  request.requestorUserName = "test-admin";
  s = sentry_client_->ListPrivilegesByUser(request, &response);
  ASSERT_TRUE(s.IsNotAuthorized()) << s.ToString();

  // List the privileges of user 'test-user' .
  request.__set_principalName("test-user");
  ASSERT_OK(sentry_client_->ListPrivilegesByUser(request, &response));
}

// Similar to above test to verify that the client can communicate with the
// Sentry service to grant roles, and errors are converted to Status
// instances.
TEST_P(SentryClientTest, TestGrantRole) {
  // Attempt to alter role by a non admin user.

  ::sentry::TSentryGroup group;
  group.groupName = "user";
  set<::sentry::TSentryGroup> groups;
  groups.insert(group);

  ::sentry::TAlterSentryRoleAddGroupsRequest group_request;
  ::sentry::TAlterSentryRoleAddGroupsResponse group_response;
  group_request.requestorUserName = "joe-interloper";
  group_request.roleName = "viewer";
  group_request.groups = groups;

  Status s = sentry_client_->AlterRoleAddGroups(group_request, &group_response);
  ASSERT_TRUE(s.IsNotAuthorized()) << s.ToString();

  // Attempt to alter a non-exist role.
  group_request.requestorUserName = "test-admin";
  s = sentry_client_->AlterRoleAddGroups(group_request, &group_response);
  ASSERT_TRUE(s.IsNotFound()) << s.ToString();

  // Alter role 'viewer' to add group 'user'.
  ::sentry::TCreateSentryRoleRequest role_request;
  role_request.requestorUserName = "test-admin";
  role_request.roleName = "viewer";
  ASSERT_OK(sentry_client_->CreateRole(role_request));
  ASSERT_OK(sentry_client_->AlterRoleAddGroups(group_request, &group_response));
}

// Similar to above test to verify that the client can communicate with the
// Sentry service to grant privileges, and errors are converted to Status
// instances.
TEST_P(SentryClientTest, TestGrantPrivilege) {
  // Alter role 'viewer' to add group 'user'.

  ::sentry::TSentryGroup group;
  group.groupName = "user";
  set<::sentry::TSentryGroup> groups;
  groups.insert(group);

  ::sentry::TAlterSentryRoleAddGroupsRequest group_request;
  ::sentry::TAlterSentryRoleAddGroupsResponse group_response;
  group_request.requestorUserName = "test-admin";
  group_request.roleName = "viewer";
  group_request.groups = groups;

  ::sentry::TCreateSentryRoleRequest role_request;
  role_request.requestorUserName = "test-admin";
  role_request.roleName = "viewer";
  ASSERT_OK(sentry_client_->CreateRole(role_request));
  ASSERT_OK(sentry_client_->AlterRoleAddGroups(group_request, &group_response));

  // Attempt to alter role by a non admin user.
  ::sentry::TAlterSentryRoleGrantPrivilegeRequest privilege_request;
  ::sentry::TAlterSentryRoleGrantPrivilegeResponse privilege_response;
  privilege_request.requestorUserName = "joe-interloper";
  privilege_request.roleName = "viewer";
  ::sentry::TSentryPrivilege privilege;
  privilege.serverName = "server";
  privilege.dbName = "db";
  privilege.tableName = "table";
  privilege.action = "SELECT";
  privilege_request.__set_privilege(privilege);
  Status s = sentry_client_->AlterRoleGrantPrivilege(privilege_request, &privilege_response);
  ASSERT_TRUE(s.IsNotAuthorized()) << s.ToString();

  // Attempt to alter a non-exist role.
  privilege_request.requestorUserName = "test-admin";
  privilege_request.roleName = "not-exist";
  s = sentry_client_->AlterRoleGrantPrivilege(privilege_request, &privilege_response);
  ASSERT_TRUE(s.IsNotFound()) << s.ToString();

  privilege_request.roleName = "viewer";
  ASSERT_OK(sentry_client_->AlterRoleGrantPrivilege(privilege_request, &privilege_response));
}

} // namespace sentry
} // namespace kudu
