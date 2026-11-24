-- Hub and dot 2026
-- Imad Jared Cabrera Trejo
-- Melissa Moreno Franca
--
-- Base de datos ProyectoIoT (MySQL/MariaDB de XAMPP). Se importa desde phpMyAdmin
-- (pestaña Importar) o con:  mysql -u root < esquema.sql
-- OJO: borra la base de datos si ya existe.
--
-- Las placas no conocen ningun id: mandan su MAC (Dispositivo.numSerie) y el numSensor
-- de cada chip, y los PHP buscan el idSensor y el idAdulto.

DROP DATABASE IF EXISTS ProyectoIoT;
CREATE DATABASE ProyectoIoT CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;
USE ProyectoIoT;

CREATE TABLE AdultoMayor (
    idAdulto   INT AUTO_INCREMENT PRIMARY KEY,
    nombre     VARCHAR(50) NOT NULL,
    apPaterno  VARCHAR(50) NOT NULL,
    apMaterno  VARCHAR(50),
    fechaNac   DATE NOT NULL
) ENGINE=InnoDB;

CREATE TABLE Cuidador (
    idCuidador   INT AUTO_INCREMENT PRIMARY KEY,
    nombre       VARCHAR(50) NOT NULL,
    apPaterno    VARCHAR(50) NOT NULL,
    apMaterno    VARCHAR(50),
    tipo         VARCHAR(30) NOT NULL,          -- familiar, enfermero, medico...
    recibeNotif  BOOLEAN NOT NULL DEFAULT TRUE
) ENGINE=InnoDB;

CREATE TABLE TelefonosCuidador (
    idCuidador  INT NOT NULL,
    telefono    VARCHAR(15) NOT NULL,
    PRIMARY KEY (idCuidador, telefono),
    FOREIGN KEY (idCuidador) REFERENCES Cuidador (idCuidador) ON DELETE CASCADE
) ENGINE=InnoDB;

CREATE TABLE Cuida (
    idCuidador  INT NOT NULL,
    idAdulto    INT NOT NULL,
    parentesco  VARCHAR(30),
    PRIMARY KEY (idCuidador, idAdulto),
    FOREIGN KEY (idCuidador) REFERENCES Cuidador (idCuidador) ON DELETE CASCADE,
    FOREIGN KEY (idAdulto) REFERENCES AdultoMayor (idAdulto) ON DELETE CASCADE
) ENGINE=InnoDB;

CREATE TABLE Dispositivo (
    idDispositivo  INT AUTO_INCREMENT PRIMARY KEY,
    idAdulto       INT,                          -- NULL = placa sin asignar
    tipo           VARCHAR(20) NOT NULL,         -- hub o dot
    numSerie       VARCHAR(32) NOT NULL UNIQUE,  -- MAC del WiFi, la imprime la placa al arrancar
    FOREIGN KEY (idAdulto) REFERENCES AdultoMayor (idAdulto) ON DELETE SET NULL
) ENGINE=InnoDB;

CREATE TABLE Sensor (
    idSensor       INT AUTO_INCREMENT PRIMARY KEY,
    idDispositivo  INT NOT NULL,
    numSensor      TINYINT UNSIGNED NOT NULL,    -- el que esta en el firmware (NUM_SENSOR_*)
    tipo           VARCHAR(30) NOT NULL,         -- MAX30102, TMP117, MPU6050
    UNIQUE (idDispositivo, numSensor),
    FOREIGN KEY (idDispositivo) REFERENCES Dispositivo (idDispositivo) ON DELETE CASCADE
) ENGINE=InnoDB;

-- Lecturas: fecha y hora se llenan solas con CURDATE() y CURTIME() al insertar.
-- La unica llave foranea es idSensor.

CREATE TABLE LecturaOxi (
    idOximetria  INT AUTO_INCREMENT PRIMARY KEY,
    spo2         TINYINT UNSIGNED NOT NULL,      -- %
    fecha        DATE NOT NULL DEFAULT (CURDATE()),
    hora         TIME NOT NULL DEFAULT (CURTIME()),
    idSensor     INT NOT NULL,
    FOREIGN KEY (idSensor) REFERENCES Sensor (idSensor) ON DELETE CASCADE
) ENGINE=InnoDB;

CREATE TABLE LecturaTemp (
    idTemperatura  INT AUTO_INCREMENT PRIMARY KEY,
    tempC          DECIMAL(4,2) NOT NULL,
    fecha          DATE NOT NULL DEFAULT (CURDATE()),
    hora           TIME NOT NULL DEFAULT (CURTIME()),
    idSensor       INT NOT NULL,
    FOREIGN KEY (idSensor) REFERENCES Sensor (idSensor) ON DELETE CASCADE
) ENGINE=InnoDB;

CREATE TABLE LecturaMov (
    idMovimiento  INT AUTO_INCREMENT PRIMARY KEY,
    acelX         DECIMAL(6,3) NOT NULL,         -- g
    acelY         DECIMAL(6,3) NOT NULL,
    acelZ         DECIMAL(6,3) NOT NULL,
    fecha         DATE NOT NULL DEFAULT (CURDATE()),
    hora          TIME NOT NULL DEFAULT (CURTIME()),
    idSensor      INT NOT NULL,
    FOREIGN KEY (idSensor) REFERENCES Sensor (idSensor) ON DELETE CASCADE
) ENGINE=InnoDB;

CREATE TABLE Alerta (
    idAlerta        INT AUTO_INCREMENT PRIMARY KEY,
    idAdulto        INT NOT NULL,
    tipo            VARCHAR(20) NOT NULL,        -- caida, ...
    valorDetectado  DECIMAL(6,2),                -- caida: pico de aceleracion en g
    fecha           DATE NOT NULL DEFAULT (CURDATE()),
    hora            TIME NOT NULL DEFAULT (CURTIME()),
    INDEX (idAdulto, tipo, fecha, hora),           -- indice normal (no es llave): para buscar caidas repetidas rapido
    FOREIGN KEY (idAdulto) REFERENCES AdultoMayor (idAdulto) ON DELETE CASCADE
) ENGINE=InnoDB;

-- Datos para probar: la adulta de prueba y las dos placas registradas

INSERT INTO AdultoMayor (idAdulto, nombre, apPaterno, apMaterno, fechaNac) VALUES
    (1, 'Maria', 'Lopez', 'Garcia', '1945-03-12');

INSERT INTO Dispositivo (idDispositivo, idAdulto, tipo, numSerie) VALUES
    (1, 1, 'dot', 'B81F3F2CA220'),
    (2, 1, 'hub', 'B81F3F2C9240');

INSERT INTO Sensor (idSensor, idDispositivo, numSensor, tipo) VALUES
    (1, 1, 1, 'TMP117'),
    (2, 1, 2, 'MPU6050'),
    (3, 2, 1, 'MAX30102');
