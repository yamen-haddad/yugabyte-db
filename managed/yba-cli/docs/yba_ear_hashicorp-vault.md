## yba ear hashicorp-vault

Manage a YugabyteDB Anywhere Hashicorp Vault encryption at rest (EAR) configuration

### Synopsis

Manage a Hashicorp Vault encryption at rest (EAR) configuration in YugabyteDB Anywhere

```
yba ear hashicorp-vault [flags]
```

### Options

```
  -n, --name string   [Optional] The name of the configuration for the action. Required for create, delete, describe, update.
  -h, --help          help for hashicorp-vault
```

### Options inherited from parent commands

```
  -a, --apiToken string    YugabyteDB Anywhere api token.
      --ca-cert string     CA certificate file path for secure connection to YugabyteDB Anywhere. Required when the endpoint is https and --insecure is not set.
      --config string      Full path to a specific configuration file for YBA CLI. If provided, this takes precedence over the directory specified via --directory, and the generated files are added to the same path. If not provided, the CLI will look for '.yba-cli.yaml' in the directory specified by --directory. Defaults to '$HOME/.yba-cli/.yba-cli.yaml'.
      --debug              Use debug mode, same as --logLevel debug.
      --directory string   Directory containing YBA CLI configuration and generated files. If specified, the CLI will look for a configuration file named '.yba-cli.yaml' in this directory. Defaults to '$HOME/.yba-cli/'.
      --disable-color      Disable colors in output. (default false)
  -H, --host string        YugabyteDB Anywhere Host (default "http://localhost:9000")
      --insecure           Allow insecure connections to YugabyteDB Anywhere. Value ignored for http endpoints. Defaults to false for https.
  -l, --logLevel string    Select the desired log level format. Allowed values: debug, info, warn, error, fatal. (default "info")
  -o, --output string      Select the desired output format. Allowed values: table, json, pretty. (default "table")
      --timeout duration   Wait command timeout, example: 5m, 1h. (default 168h0m0s)
      --wait               Wait until the task is completed, otherwise it will exit immediately. (default true)
```

### SEE ALSO

* [yba ear](yba_ear.md)	 - Manage YugabyteDB Anywhere Encryption at Rest Configurations
* [yba ear hashicorp-vault create](yba_ear_hashicorp-vault_create.md)	 - Create a YugabyteDB Anywhere Hashicorp Vault encryption at rest configuration
* [yba ear hashicorp-vault delete](yba_ear_hashicorp-vault_delete.md)	 - Delete a YugabyteDB Anywhere Hashicorp Vault encryption at rest configuration
* [yba ear hashicorp-vault describe](yba_ear_hashicorp-vault_describe.md)	 - Describe a Hashicorp Vault YugabyteDB Anywhere Encryption In Transit (EAR) configuration
* [yba ear hashicorp-vault list](yba_ear_hashicorp-vault_list.md)	 - List Hashicorp Vault YugabyteDB Anywhere Encryption In Transit (EAR) configurations
* [yba ear hashicorp-vault refresh](yba_ear_hashicorp-vault_refresh.md)	 - Refresh a Hashicorp Vault YugabyteDB Anywhere Encryption In Transit (EAR) configuration
* [yba ear hashicorp-vault update](yba_ear_hashicorp-vault_update.md)	 - Update a YugabyteDB Anywhere Hashicorp Vault encryption at rest (EAR) configuration

