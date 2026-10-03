-- The entrypoint has already created MYSQL_USER with the server default,
-- caching_sha2_password, which react/mysql 0.6 cannot answer. Re-create it
-- with the plugin the client does implement; --mysql-native-password=ON in
-- compose.yaml is what makes the plugin available to name here.
--
-- The password is repeated as a literal because initdb scripts get no
-- environment substitution; it must match MYSQL_PASSWORD in compose.yaml.
ALTER USER 'fpmng'@'%' IDENTIFIED WITH mysql_native_password BY 'fpmng';
FLUSH PRIVILEGES;
